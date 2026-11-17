// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "s3_config.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include "tests/temporary_directory.hpp"
#include <optional>
#include <utility>
#include <vector>

namespace snapshot::pagebroker {
namespace {
class WorkloadIdentityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const auto* name : {"SNAPSHOT_S3_CREDENTIALS_SOURCE", "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
          "AWS_CONTAINER_CREDENTIALS_FULL_URI", "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
          "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", "AWS_WEB_IDENTITY_TOKEN_FILE", "AWS_ROLE_ARN",
          "AWS_SHARED_CREDENTIALS_FILE", "AWS_CONFIG_FILE", "AWS_REGION", "AWS_DEFAULT_REGION",
          "AWS_EC2_METADATA_DISABLED", "AWS_CA_BUNDLE", "RUNAI_STREAMER_S3_USE_VIRTUAL_ADDRESSING"}) {
      const auto* value = std::getenv(name);
      saved.push_back({name, value ? std::optional<std::string>(value) : std::nullopt});
      unsetenv(name);
    }
  }
  void TearDown() override {
    for (const auto& [name, value] : saved) {
      if (value) setenv(name.c_str(), value->c_str(), 1);
      else unsetenv(name.c_str());
    }
  }
  S3Connection Identity() {
    setenv("SNAPSHOT_S3_CREDENTIALS_SOURCE", "workloadIdentity", 1);
    return ParseS3Config(R"({"storeId":"test","bucket":"test-bucket","region":"test-region",
      "endpoint":"https://s3.example.com","credentialsSource":"workloadIdentity"})").transfer.connection;
  }
  std::vector<std::pair<std::string, std::optional<std::string>>> saved;
};
TEST_F(WorkloadIdentityTest, ContainerIdentityDoesNotResolveOrPinCredentials) {
  setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://credentials.example.com", 1);
  setenv("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", "/projected/token", 1);
  setenv("AWS_SHARED_CREDENTIALS_FILE", "/missing/credentials", 1);
  const auto connection = LoadS3Credentials(Identity());
  EXPECT_TRUE(connection.use_default_credentials);
  EXPECT_TRUE(connection.access_key_id.empty());
  EXPECT_TRUE(connection.secret_access_key.empty());
  EXPECT_TRUE(connection.session_token.empty());
  S3Config config;
  config.transfer.connection = connection;
  model_streamer::settings::ConfigureEnvironment({}, &config);
  EXPECT_STREQ(std::getenv("AWS_SHARED_CREDENTIALS_FILE"), "/dev/null");
  EXPECT_STREQ(std::getenv("AWS_CONFIG_FILE"), "/dev/null");
  EXPECT_STREQ(std::getenv("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE"), "/projected/token");
  EXPECT_STREQ(std::getenv("AWS_EC2_METADATA_DISABLED"), "true");
}
TEST_F(WorkloadIdentityTest, RequiresEndpointAndRotatingTokenFile) {
  setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://credentials.example.com", 1);
  EXPECT_THROW(LoadS3Credentials(Identity()), std::invalid_argument);
  unsetenv("AWS_CONTAINER_CREDENTIALS_FULL_URI");
  setenv("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", "/projected/token", 1);
  EXPECT_THROW(LoadS3Credentials(Identity()), std::invalid_argument);
}
TEST_F(WorkloadIdentityTest, RejectsMissingIdentityAndStaticOverrides) {
  EXPECT_THROW(LoadS3Credentials(Identity()), std::invalid_argument);
  setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://credentials.example.com", 1);
  setenv("AWS_ACCESS_KEY_ID", "fake-key", 1);
  EXPECT_THROW(LoadS3Credentials(Identity()), std::invalid_argument);
  auto mixed = Identity();
  mixed.access_key_id = "fake-key";
  mixed.secret_access_key = "fake-secret";
  EXPECT_THROW(mixed.Validate(), std::invalid_argument);
}
TEST_F(WorkloadIdentityTest, FileModeStillRequiresConfiguredFile) {
  setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://credentials.example.com", 1);
  auto file = Identity();
  file.use_default_credentials = false;
  EXPECT_THROW(LoadS3Credentials(file), std::invalid_argument);
}
TEST_F(WorkloadIdentityTest, PodIdentityModeWorksWithStaleFileConfig) {
  setenv("SNAPSHOT_S3_CREDENTIALS_SOURCE", "workloadIdentity", 1);
  setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://credentials.example.com", 1);
  setenv("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", "/projected/token", 1);
  for (const auto* source : {"file", "workloadIdentity"}) {
    auto config = std::string(R"({"storeId":"test","bucket":"test-bucket","region":"test-region",
      "endpoint":"https://s3.example.com","credentialsSource":")") + source + R"("})";
    EXPECT_TRUE(LoadS3Credentials(ParseS3Config(config).transfer.connection).use_default_credentials);
  }
}
TEST_F(WorkloadIdentityTest, PodFileModeWorksWithStaleIdentityConfig) {
  test::TemporaryDirectory dir;
  const auto file = dir.path() / "credentials";
  std::ofstream(file) << "[default]\naws_access_key_id=file-key\naws_secret_access_key=fake-secret\n";
  setenv("AWS_SHARED_CREDENTIALS_FILE", file.c_str(), 1);
  for (const auto* mode : {"", "file"}) {
    setenv("SNAPSHOT_S3_CREDENTIALS_SOURCE", mode, 1);
    auto connection = ParseS3Config(R"({"storeId":"test","bucket":"test-bucket","region":"test-region",
      "endpoint":"https://s3.example.com","credentialsSource":"workloadIdentity"})").transfer.connection;
    connection = LoadS3Credentials(connection);
    EXPECT_FALSE(connection.use_default_credentials);
    EXPECT_EQ(connection.access_key_id, "file-key");
  }
}
TEST_F(WorkloadIdentityTest, RejectsUnknownCredentialSource) {
  setenv("SNAPSHOT_S3_CREDENTIALS_SOURCE", "unknown", 1);
  EXPECT_THROW(ParseS3Config(R"({"storeId":"test","bucket":"test-bucket","region":"test-region",
      "endpoint":"https://s3.example.com","credentialsSource":"unknown"})"), std::invalid_argument);
}
} // namespace
} // namespace snapshot::pagebroker
