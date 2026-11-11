/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
namespace oceanbase {
namespace common { class ObMySQLProxy; }
namespace query {
int start_ai_column_worker(common::ObMySQLProxy &proxy);
void stop_ai_column_worker();
void wait_ai_column_worker();
}
}
