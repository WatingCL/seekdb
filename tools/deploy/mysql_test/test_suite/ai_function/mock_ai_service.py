#!/usr/bin/env python3
"""Deterministic mock AI service for the ai_function mysqltest suite.

The suite talks to this fixture over HTTP instead of a real model service.
Start it before running the suite:

    python3 tools/deploy/mysql_test/test_suite/ai_function/mock_ai_service.py

It listens on 0.0.0.0:8000 and implements the protocols used by the AI
functions:

    POST /compatible-mode/v1/chat/completions   OpenAI chat completion
    POST /compatible-mode/v1/embeddings          OpenAI embeddings
    POST /v1/rerank                              SiliconFlow style rerank
    POST /api/generate                           native Ollama generation
    POST /api/embed                              native Ollama embedding

The fixture is intentionally strict so that protocol regressions are caught:
requests must carry a JSON content type, OpenAI style endpoints must carry a
bearer token, native Ollama endpoints must not carry one, and native Ollama
generation must disable streaming. Embedding and rerank answers are returned
in reverse order on purpose so that the client has to restore the original
order by the index field.
"""

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DIMENSION = 768


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = []
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def respond(self, value, status=200):
        payload = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/reset":
            with self.lock:
                self.requests.clear()
        self.respond(self.statistics())

    @classmethod
    def statistics(cls):
        with cls.lock:
            inputs = []
            for path, body in cls.requests:
                value = body.get("input")
                if isinstance(value, list):
                    inputs.append(len(value))
            return {
                "request_count": len(cls.requests),
                "max_embed_batch": max(inputs) if inputs else 0,
            }

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
            if not isinstance(body, dict):
                raise ValueError("request must be an object")
            if self.headers.get_content_type() != "application/json":
                raise ValueError("JSON content type is required")
            if not body.get("model"):
                raise ValueError("model is required")
            if self.path.startswith("/api/"):
                if self.headers.get("Authorization"):
                    raise ValueError("native Ollama requests must not carry authorization")
            elif not self.headers.get("Authorization", "").startswith("Bearer "):
                raise ValueError("OpenAI style requests require a bearer token")

            with self.lock:
                self.requests.append((self.path, body))

            if "messages" in body or "prompt" in body:
                self.respond(self.complete(body))
            elif "input" in body:
                self.respond(self.embed(body))
            elif "documents" in body:
                self.respond(self.rerank(body))
            else:
                raise ValueError("unknown request")
        except (KeyError, TypeError, ValueError) as error:
            self.respond({"error": str(error)}, 400)

    def complete(self, body):
        if self.path.startswith("/api/"):
            if body.get("stream") is not False:
                raise ValueError("native Ollama generation must disable streaming")
            prompt = body.get("prompt", "")
            return {"model": body.get("model", ""), "response": "mock ollama: %s" % prompt, "done": True}
        messages = body.get("messages") or []
        content = messages[-1].get("content", "") if messages else ""
        return {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "model": body.get("model", ""),
            "choices": [{"index": 0, "message": {"role": "assistant", "content": "mock completion: %s" % content}, "finish_reason": "stop"}],
        }

    def embed(self, body):
        inputs = body.get("input")
        if isinstance(inputs, str):
            inputs = [inputs]
        if not isinstance(inputs, list):
            raise ValueError("input must be a string or an array of strings")
        dimension = body.get("dimensions", DEFAULT_DIMENSION)
        if not isinstance(dimension, int) or isinstance(dimension, bool) or dimension <= 0:
            raise ValueError("invalid dimensions")
        embeddings = []
        for value in inputs:
            text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
            vector = [round(((len(text.encode("utf-8")) * 31 + i) % 1000) / 1000.0, 6) for i in range(dimension)]
            embeddings.append(vector)
        if self.path.startswith("/api/"):
            return {"model": body.get("model", ""), "embeddings": embeddings}
        data = [{"object": "embedding", "index": index, "embedding": vector} for index, vector in enumerate(embeddings)]
        # Faults are selected by the test endpoint URL, never by user/model data.
        if self.path.startswith("/fault/"):
            fault = self.path.split("/")[2]
            if fault == "missing-index" and data:
                data[0].pop("index")
            elif fault == "duplicate-index" and len(data) > 1:
                data[1]["index"] = data[0]["index"]
            elif fault == "invalid-index" and data:
                data[0]["index"] = "0"
            elif fault == "wrong-count":
                data = data[:-1]
            elif fault == "empty-vector" and data:
                data[0]["embedding"] = []
            elif fault == "inconsistent-dimension" and data:
                data[0]["embedding"] = data[0]["embedding"][:-1]
            elif fault == "nonnumeric" and data:
                data[0]["embedding"][0] = "invalid"
            elif fault == "overflow" and data:
                data[0]["embedding"][0] = 1e100
        # reversed on purpose: the client has to restore the order by index
        return {"object": "list", "model": body.get("model", ""), "data": list(reversed(data))}

    def rerank(self, body):
        documents = body.get("documents") or []
        if not isinstance(documents, list):
            raise ValueError("documents must be an array")
        query = body.get("query", "")
        results = []
        for index, document in enumerate(documents):
            if isinstance(document, dict):
                text = ""
                for key in ("text", "content"):
                    if isinstance(document.get(key), str):
                        text = document[key]
                        break
            else:
                text = str(document)
            score = 1.0 if text == query else (0.5 if query and query in text else 0.01)
            results.append({"index": index, "relevance_score": score, "document": {"text": text}})
        if self.path.startswith("/fault/"):
            fault = self.path.split("/")[2]
            if fault == "wrong-count":
                results = results[:-1]
            elif fault == "duplicate-index" and len(results) > 1:
                results[1]["index"] = results[0]["index"]
            elif fault == "out-of-range" and results:
                results[0]["index"] = len(results)
            elif fault == "missing-score" and results:
                results[0].pop("relevance_score")
            elif fault == "invalid-results":
                return {"results": {"unexpected": "object"}}
        # reversed on purpose: the client has to restore the order by index
        return {"id": "rerank-mock", "results": list(reversed(results))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--dim", type=int, default=DEFAULT_DIMENSION)
    arguments = parser.parse_args()
    server = ThreadingHTTPServer((arguments.host, arguments.port), Handler)
    print("mock AI service listening on %s:%d" % (arguments.host, arguments.port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
