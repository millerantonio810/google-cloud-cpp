// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/client.h"

namespace gcs = google::cloud::storage;

int main(int argc, char* argv[]) try {
  // Make sure the arguments are valid.
  if (argc != 3) {
    std::string const cmd = argv[0];
    // Until std::filesystem is widely available this hack works for most paths.
    auto last_slash = std::string(argv[0]).find_last_of("/\\");
    std::cerr << "Usage: " << cmd.substr(last_slash + 1)
              << " <bucket-name> <object-name>\n";
    return 1;
  }
  std::string const bucket_name = argv[1];
  std::string const object_name = argv[2];

  google::cloud::StatusOr<gcs::Client> client =
      gcs::Client::CreateDefaultClient();
  if (!client) {
    std::cerr << "Could not create default GCS Client, status="
              << client.status() << "\n";
    return 1;
  }

  gcs::ObjectWriteStream os = client->WriteObject(bucket_name, object_name);
  os.exceptions(std::ios_base::badbit | std::ios_base::failbit);
  os << "Hello World\n";
  os.Close();
  gcs::ObjectMetadata meta = os.metadata().value();
  std::cout << "Successfully created object, generation=" << meta.generation()
            << "\n";

  gcs::ObjectReadStream stream = client->ReadObject(bucket_name, object_name);
  stream.exceptions(std::ios_base::badbit);

  int count = 0;
  std::string line;
  while (std::getline(stream, line, '\n')) {
    ++count;
  }
  google::cloud::Status status = client->DeleteObject(
      bucket_name, object_name, gcs::Generation(meta.generation()));
  if (!status.ok()) {
    throw std::runtime_error(status.message());
  }

  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception raised: " << ex.what() << "\n";
  return 1;
}
