// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "s3_config.hpp"
#ifdef IDENTITY_NATIVE_RESTORE
#include "transfer/engine/model_streamer/model_streamer_restore.hpp"
#endif
#include <fstream>
#include <iostream>

using namespace snapshot::pagebroker;
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  S3Connection connection;
  connection.region = "us-east-1";
  connection.endpoint = argv[1];
  connection.use_virtual_addressing = false;
  connection.use_default_credentials = true;
  connection = LoadS3Credentials(connection);
  const std::filesystem::path root = argv[2];
  const Cancellation deadline(Cancellation::Clock::now() + std::chrono::seconds(12));
  try {
#ifdef IDENTITY_NATIVE_RESTORE
    S3Config config;
    config.transfer.connection = connection;
    model_streamer::settings::ConfigureEnvironment({}, &config);
    ModelStreamerRestore restore({connection.region, connection.endpoint, {}, {}, {}}, std::chrono::seconds(8));
    for (const auto* name : {"a", "b"}) {
      CheckpointInventory inventory;
      inventory.root_permissions = std::filesystem::perms::owner_all;
      inventory.files.push_back({std::string("s3://bucket/prefix/") + name, name, 8, std::filesystem::perms::owner_read});
      restore.Stage(inventory, root / name, deadline);
      std::ifstream input(root / name / name);
      std::string bytes;
      std::getline(input, bytes);
      if (bytes != "snapshot") throw std::runtime_error("restored bytes differ");
    }
#else
    std::ofstream(root / "a") << "snapshot";
    std::ofstream(root / "b") << "snapshot";
    S3Limits limits;
    limits.request_retries = 0;
    limits.connect_timeout = std::chrono::seconds(2);
    limits.request_timeout = std::chrono::seconds(2);
    S3Client client(connection, limits);
    client.UploadFiles(root, {"bucket", "prefix"}, {{"a", 8}, {"b", 8}}, deadline);
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
