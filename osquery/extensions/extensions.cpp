/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <csignal>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <osquery/core.h>
#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/registry.h>
#include <osquery/sql.h>
#include <osquery/system.h>

#include "osquery/core/conversions.h"
#include "osquery/core/process.h"
#include "osquery/core/watcher.h"
#include "osquery/extensions/interface.h"
#include "osquery/filesystem/fileops.h"

using namespace osquery::extensions;

namespace fs = boost::filesystem;

namespace osquery {

// Millisecond latency between initalizing manager pings.
const size_t kExtensionInitializeLatency = 20;

#ifdef __APPLE__
#define MODULE_EXTENSION ".dylib"
#define EXT_EXTENSION ".ext"
#elif defined(WIN32)
#define MODULE_EXTENSION ".dll"
#define EXT_EXTENSION ".exe"
#else
#define MODULE_EXTENSION ".so"
#define EXT_EXTENSION ".ext"
#endif

enum ExtenableTypes {
  EXTENSION = 1,
  MODULE = 2,
};

const std::map<ExtenableTypes, std::string> kExtendables = {
    {EXTENSION, EXT_EXTENSION}, {MODULE, MODULE_EXTENSION},
};

CLI_FLAG(bool, disable_extensions, false, "Disable extension API");

CLI_FLAG(string,
         extensions_socket,
         OSQUERY_SOCKET "osquery.em",
         "Path to the extensions UNIX domain socket")

CLI_FLAG(string,
         extensions_autoload,
         OSQUERY_HOME "/extensions.load",
         "Optional path to a list of autoloaded & managed extensions")

CLI_FLAG(string,
         extensions_timeout,
         "3",
         "Seconds to wait for autoloaded extensions");

CLI_FLAG(string,
         extensions_interval,
         "3",
         "Seconds delay between connectivity checks")

CLI_FLAG(string,
         modules_autoload,
         OSQUERY_HOME "/modules.load",
         "Optional path to a list of autoloaded registry modules")

SHELL_FLAG(string, extension, "", "Path to a single extension to autoload");

CLI_FLAG(string,
         extensions_require,
         "",
         "Comma-separated list of required extensions");

/**
 * @brief Alias the extensions_socket (used by core) to a simple 'socket'.
 *
 * Extension binaries will more commonly set the path to an extension manager
 * socket. Alias the long switch name to 'socket' for an easier UX.
 *
 * We include timeout and interval, where the 'extensions_' prefix is removed
 * in the alias since we are already within the context of an extension.
 */
EXTENSION_FLAG_ALIAS(socket, extensions_socket);
EXTENSION_FLAG_ALIAS(timeout, extensions_timeout);
EXTENSION_FLAG_ALIAS(interval, extensions_interval);

#ifdef WIN32
// Time to wait for a busy named pipe, if it exists
#define NAMED_PIPE_WAIT 500

/**
 * We cannot use existing methods to determine the lifespan of the
 * extensions/extensions manager socket. On Windows, the Thrift install is
 * brittle and does not like a quick connect and disconnect. To compensate, we
 * use WaitNamedPipe to determine the existence of a named pipe. If the named
 * pipe does not exist, WaitNamedPipe should error with ERROR_BAD_PATHNAME.
 */
static Status isNamedPipePathValid(const std::string& path) {
  if (!boost::starts_with(path, OSQUERY_SOCKET)) {
    return Status(1, "Bad named pipe name prefix");
  }

  if ((::WaitNamedPipeA(path.c_str(), NAMED_PIPE_WAIT) == 0) &&
      (::GetLastError() == ERROR_BAD_PATHNAME)) {
    return Status(1, "Named pipe path is invalid");
  }

  return Status(0, "OK");
}
#endif

Status applyExtensionDelay(std::function<Status(bool& stop)> predicate) {
  // Make sure the extension manager path exists, and is writable.
  size_t delay = 0;
  // The timeout is given in seconds, but checked interval is microseconds.
  size_t timeout = atoi(FLAGS_extensions_timeout.c_str()) * 1000;
  if (timeout < kExtensionInitializeLatency * 10) {
    timeout = kExtensionInitializeLatency * 10;
  }

  Status status;
  do {
    bool stop = false;
    status = predicate(stop);
    if (stop || status.ok()) {
      break;
    }

    // Increase the total wait detail.
    delay += kExtensionInitializeLatency;
    sleepFor(kExtensionInitializeLatency);
  } while (delay < timeout);
  return status;
}

Status extensionPathActive(const std::string& path, bool use_timeout = false) {
  return applyExtensionDelay(([path, &use_timeout](bool& stop) {
#ifdef WIN32
    // This makes sure the pipe exists in some capacity (could be busy at the
    // moment)
    if (namedPipeExists(path).ok()) {
      return Status(0, "OK");
    }
#else
    if (pathExists(path) && isWritable(path)) {
      try {
        ExtensionStatus status;
        auto client = EXManagerClient(path);
        return Status(0, "OK");
      } catch (const std::exception& e) {
        // Path might exist without a connected extension or extension manager.
      }
    }
#endif
    // Only check active once if this check does not allow a timeout.
    if (!use_timeout) {
      stop = true;
    }
    return Status(1, "Extension socket not available: " + path);
  }));
}

void ExtensionWatcher::start() {
  // Watch the manager, if the socket is removed then the extension will die.
  // A check for sane paths and activity is applied before the watcher
  // service is added and started.
  while (!interrupted()) {
    watch();
    pauseMilli(interval_);
  }
}

void ExtensionManagerWatcher::start() {
  // Watch each extension.
  while (!interrupted()) {
    watch();
    pauseMilli(interval_);
  }

  // When interrupted, request each extension tear down.
  const auto uuids = Registry::routeUUIDs();
  for (const auto& uuid : uuids) {
    try {
      auto path = getExtensionSocket(uuid);
      auto client = EXClient(path);
      client.get()->shutdown();
    } catch (const std::exception& /* e */) {
      VLOG(1) << "Extension UUID " << uuid << " shutdown request failed";
      continue;
    }
  }
}

void ExtensionWatcher::exitFatal(int return_code) {
  // Exit the extension.
  // We will save the wanted return code and raise an interrupt.
  // This interrupt will be handled by the main thread then join the watchers.
  Initializer::requestShutdown(return_code);
}

void ExtensionWatcher::watch() {
  // Attempt to ping the extension core.
  // This does NOT use pingExtension to avoid the latency checks applied.
  ExtensionStatus status;
  bool core_sane = true;
#ifdef WIN32
  // Check to see if the pipe name is a valid named pipe
  if (!namedPipeExists(path_).ok()) {
    core_sane = false;
  }
#else
  if (isWritable(path_)) {
    try {
      auto client = EXManagerClient(path_);
      // Ping the extension manager until it goes down.
      client.get()->ping(status);

    } catch (const std::exception& e) {
      core_sane = false;
    }
  } else {
    // The previously-writable extension socket is not usable.
    core_sane = false;
  }
#endif

  if (!core_sane) {
    LOG(INFO) << "Extension watcher ending: osquery core has gone away";
    exitFatal(0);
  }

  if (status.code != ExtensionCode::EXT_SUCCESS && fatal_) {
    // The core may be healthy but return a failed ping status.
    exitFatal();
  }
}

void ExtensionManagerWatcher::watch() {
  // Watch the set of extensions, if the socket is removed then the extension
  // will be deregistered.
  const auto uuids = Registry::routeUUIDs();

  ExtensionStatus status;
  for (const auto& uuid : uuids) {
    auto path = getExtensionSocket(uuid);
#ifdef WIN32
    // Check to see if the pipe name is a valid named pipe
    if (!namedPipeExists(path).ok()) {
      LOG(INFO) << "Extension UUID " << uuid << " ping failed";

      // Immediate fail non-writable paths.
      failures_[uuid] += 1;
    }
#else
    // The manager first checks writeability of the extension socket.
    auto writable = isWritable(path);
    if (!writable && failures_[uuid] == 0) {
      // If there was never a failure then this is the first attempt.
      // Allow the extension to be latent and respect the autoload timeout.
      VLOG(1) << "Extension UUID " << uuid << " initial check failed";
      writable = extensionPathActive(path, true);
    }

    // All extensions will have a single failure (and odd use of the counting).
    // If failures get to 2 then the extension will be removed.
    failures_[uuid] = 1;
    if (writable) {
      try {
        auto client = EXClient(path);
        // Ping the extension until it goes down.
        client.get()->ping(status);
      } catch (const std::exception& e) {
        failures_[uuid] += 1;
        continue;
      }
    } else {
      // Immediate fail non-writable paths.
      failures_[uuid] += 1;
      continue;
    }

    if (status.code != ExtensionCode::EXT_SUCCESS) {
      LOG(INFO) << "Extension UUID " << uuid << " ping failed";
      failures_[uuid] += 1;
    } else {
      failures_[uuid] = 1;
    }
#endif
  }

  for (const auto& uuid : failures_) {
    if (uuid.second > 1) {
      LOG(INFO) << "Extension UUID " << uuid.first << " has gone away";
      Registry::removeBroadcast(uuid.first);
      failures_[uuid.first] = 1;
    }
  }
}

Status socketWritable(const fs::path& path) {
  if (pathExists(path).ok()) {
    if (!isWritable(path).ok()) {
      return Status(1, "Cannot write extension socket: " + path.string());
    }

    if (!osquery::remove(path).ok()) {
      return Status(1, "Cannot remove extension socket: " + path.string());
    }
  } else {
    if (!pathExists(path.parent_path()).ok()) {
      return Status(1, "Extension socket directory missing: " + path.string());
    }

    if (!isWritable(path.parent_path()).ok()) {
      return Status(1, "Cannot create extension socket: " + path.string());
    }
  }

  return Status(0, "OK");
}

void loadExtensions() {
  // Disabling extensions will disable autoloading.
  if (FLAGS_disable_extensions) {
    return;
  }

  // Optionally autoload extensions, sanitize the binary path and inform
  // the osquery watcher to execute the extension when started.
  auto status = loadExtensions(
      fs::path(FLAGS_extensions_autoload).make_preferred().string());
  if (!status.ok()) {
    VLOG(1) << "Could not autoload extensions: " << status.what();
  }
}

void loadModules() {
  if (isPlatform(PlatformType::TYPE_WINDOWS)) {
    VLOG(1) << "Windows does not support loadable modules";
    return;
  }

  auto status =
      loadModules(fs::path(FLAGS_modules_autoload).make_preferred().string());
  if (!status.ok()) {
    VLOG(1) << "Could not autoload modules: " << status.what();
  }
}

static bool isFileSafe(std::string& path, ExtenableTypes type) {
  boost::trim(path);
  // A 'type name' may be used in verbose log output.
  std::string type_name = ((type == EXTENSION) ? "extension" : "module");
  if (path.size() == 0 || path[0] == '#' || path[0] == ';') {
    return false;
  }

  // Resolve acceptable extension binaries from autoload paths.
  if (isDirectory(path).ok()) {
    VLOG(1) << "Cannot autoload " << type_name << " from directory: " << path;
    return false;
  }
  // The extendables will force an appropriate file path extension.
  auto& ext = kExtendables.at(type);

  // Only autoload file which were safe at the time of discovery.
  // If the binary later becomes unsafe (permissions change) then it will fail
  // to reload if a reload is ever needed.
  fs::path extendable(path);
  // Set the output sanitized path.
  path = extendable.string();
  if (!safePermissions(extendable.parent_path().string(), path, true)) {
    LOG(WARNING) << "Will not autoload " << type_name
                 << " with unsafe directory permissions: " << path;
    return false;
  }

  if (extendable.extension().string() != ext) {
    LOG(WARNING) << "Will not autoload " << type_name << " not ending in '"
                 << ext << "': " << path;
    return false;
  }

  VLOG(1) << "Found autoloadable " << type_name << ": " << path;
  return true;
}

Status loadExtensions(const std::string& loadfile) {
  std::string autoload_paths;
  if (!FLAGS_extension.empty()) {
    // This is a shell-only development flag for quickly loading/using a single
    // extension. It bypasses the safety check.
    Watcher::addExtensionPath(FLAGS_extension);
  }

  if (readFile(loadfile, autoload_paths).ok()) {
    for (auto& path : osquery::split(autoload_paths, "\n")) {
      if (isFileSafe(path, EXTENSION)) {
        // After the path is sanitized the watcher becomes responsible for
        // forking and executing the extension binary.
        Watcher::addExtensionPath(path);
      }
    }
    return Status(0, "OK");
  }
  return Status(1, "Failed reading: " + loadfile);
}

Status loadModules(const std::string& loadfile) {
  // Split the search path for modules using a ':' delimiter.
  bool all_loaded = true;
  std::string autoload_paths;
  if (readFile(loadfile, autoload_paths).ok()) {
    for (auto& path : osquery::split(autoload_paths, "\n")) {
      if (isFileSafe(path, MODULE)) {
        RegistryModuleLoader loader(path);
        loader.init();
      } else {
        all_loaded = false;
      }
    }
    // Return an aggregate failure if any load fails (invalid search path).
    return Status((all_loaded) ? 0 : 1);
  }
  return Status(1, "Failed reading: " + loadfile);
}

Status startExtension(const std::string& name, const std::string& version) {
  return startExtension(name, version, "0.0.0");
}

Status startExtension(const std::string& name,
                      const std::string& version,
                      const std::string& min_sdk_version) {
  // Tell the registry that this is an extension.
  // When a broadcast is requested this registry should not send core plugins.
  Registry::setExternal();

  // Latency converted to milliseconds, used as a thread interruptible.
  auto latency = atoi(FLAGS_extensions_interval.c_str()) * 1000;
  auto status = startExtensionWatcher(FLAGS_extensions_socket, latency, true);
  if (!status.ok()) {
    // If the threaded watcher fails to start, fail the extension.
    return status;
  }

  status = startExtension(
      FLAGS_extensions_socket, name, version, min_sdk_version, kSDKVersion);
  if (!status.ok()) {
    // If the extension failed to start then the EM is most likely unavailable.
    return status;
  }
  return Status(0);
}

Status startExtension(const std::string& manager_path,
                      const std::string& name,
                      const std::string& version,
                      const std::string& min_sdk_version,
                      const std::string& sdk_version) {
  // Make sure the extension manager path exists, and is writable.
  auto status = extensionPathActive(manager_path, true);
  if (!status.ok()) {
    return status;
  }

  // The Registry broadcast is used as the ExtensionRegistry.
  auto broadcast = Registry::getBroadcast();
  // The extension will register and provide name, version, sdk details.
  InternalExtensionInfo info;
  info.name = name;
  info.version = version;
  info.sdk_version = sdk_version;
  info.min_sdk_version = min_sdk_version;

  // If registration is successful, we will also request the manager's options.
  InternalOptionList options;
  // Register the extension's registry broadcast with the manager.
  ExtensionStatus ext_status;
  try {
    auto client = EXManagerClient(manager_path);
    client.get()->registerExtension(ext_status, info, broadcast);
    // The main reason for a failed registry is a duplicate extension name
    // (the extension process is already running), or the extension broadcasts
    // a duplicate registry item.
    if (ext_status.code != ExtensionCode::EXT_SUCCESS) {
      return Status(ext_status.code, ext_status.message);
    }
    // Request the core options, mainly to set the active registry plugins for
    // logger and config.
    client.get()->options(options);
  } catch (const std::exception& e) {
    return Status(1, "Extension register failed: " + std::string(e.what()));
  }

  // Now that the uuid is known, try to clean up stale socket paths.
  auto extension_path = getExtensionSocket(ext_status.uuid, manager_path);

#ifdef WIN32
  status = isNamedPipePathValid(extension_path);
#else
  status = socketWritable(extension_path);
#endif

  if (!status) {
    return status;
  }

  // Set the active config and logger plugins. The core will arbitrate if the
  // plugins are not available in the extension's local registry.
  Registry::setActive("config", options["config_plugin"].value);
  Registry::setActive("logger", options["logger_plugin"].value);
  Registry::setActive("distributed", options["distributed_plugin"].value);
  // Set up all lazy registry plugins and the active config/logger plugin.
  Registry::setUp();

  // Start the extension's Thrift server
  Dispatcher::addService(
      std::make_shared<ExtensionRunner>(manager_path, ext_status.uuid));
  VLOG(1) << "Extension (" << name << ", " << ext_status.uuid << ", " << version
          << ", " << sdk_version << ") registered";
  return Status(0, std::to_string(ext_status.uuid));
}

Status queryExternal(const std::string& manager_path,
                     const std::string& query,
                     QueryData& results) {
  // Make sure the extension path exists, and is writable.
  auto status = extensionPathActive(manager_path);
  if (!status.ok()) {
    return status;
  }

  ExtensionResponse response;
  try {
    auto client = EXManagerClient(manager_path);
    client.get()->query(response, query);
  } catch (const std::exception& e) {
    return Status(1, "Extension call failed: " + std::string(e.what()));
  }

  for (const auto& row : response.response) {
    results.push_back(row);
  }

  return Status(response.status.code, response.status.message);
}

Status queryExternal(const std::string& query, QueryData& results) {
  return queryExternal(FLAGS_extensions_socket, query, results);
}

Status getQueryColumnsExternal(const std::string& manager_path,
                               const std::string& query,
                               TableColumns& columns) {
  // Make sure the extension path exists, and is writable.
  auto status = extensionPathActive(manager_path);
  if (!status.ok()) {
    return status;
  }

  ExtensionResponse response;
  try {
    auto client = EXManagerClient(manager_path);
    client.get()->getQueryColumns(response, query);
  } catch (const std::exception& e) {
    return Status(1, "Extension call failed: " + std::string(e.what()));
  }

  // Translate response map: {string: string} to a vector: pair(name, type).
  for (const auto& column : response.response) {
    for (const auto& col : column) {
      columns.push_back(std::make_tuple(
          col.first, columnTypeName(col.second), ColumnOptions::DEFAULT));
    }
  }

  return Status(response.status.code, response.status.message);
}

Status getQueryColumnsExternal(const std::string& query,
                               TableColumns& columns) {
  return getQueryColumnsExternal(FLAGS_extensions_socket, query, columns);
}

Status pingExtension(const std::string& path) {
  if (FLAGS_disable_extensions) {
    return Status(1, "Extensions disabled");
  }

  // Make sure the extension path exists, and is writable.
  auto status = extensionPathActive(path);
  if (!status.ok()) {
    return status;
  }

  ExtensionStatus ext_status;
  try {
    auto client = EXClient(path);
    client.get()->ping(ext_status);
  } catch (const std::exception& e) {
    return Status(1, "Extension call failed: " + std::string(e.what()));
  }

  return Status(ext_status.code, ext_status.message);
}

Status getExtensions(ExtensionList& extensions) {
  if (FLAGS_disable_extensions) {
    return Status(1, "Extensions disabled");
  }
  return getExtensions(FLAGS_extensions_socket, extensions);
}

Status getExtensions(const std::string& manager_path,
                     ExtensionList& extensions) {
  // Make sure the extension path exists, and is writable.
  auto status = extensionPathActive(manager_path);
  if (!status.ok()) {
    return status;
  }

  InternalExtensionList ext_list;
  try {
    auto client = EXManagerClient(manager_path);
    client.get()->extensions(ext_list);
  } catch (const std::exception& e) {
    return Status(1, "Extension call failed: " + std::string(e.what()));
  }

  // Add the extension manager to the list called (core).
  extensions[0] = {"core", kVersion, "0.0.0", kSDKVersion};

  // Convert from Thrift-internal list type to RouteUUID/ExtenionInfo type.
  for (const auto& ext : ext_list) {
    extensions[ext.first] = {ext.second.name,
                             ext.second.version,
                             ext.second.min_sdk_version,
                             ext.second.sdk_version};
  }

  return Status(0, "OK");
}

Status callExtension(const RouteUUID uuid,
                     const std::string& registry,
                     const std::string& item,
                     const PluginRequest& request,
                     PluginResponse& response) {
  if (FLAGS_disable_extensions) {
    return Status(1, "Extensions disabled");
  }
  return callExtension(
      getExtensionSocket(uuid), registry, item, request, response);
}

Status callExtension(const std::string& extension_path,
                     const std::string& registry,
                     const std::string& item,
                     const PluginRequest& request,
                     PluginResponse& response) {
  // Make sure the extension manager path exists, and is writable.
  auto status = extensionPathActive(extension_path);
  if (!status.ok()) {
    return status;
  }

  ExtensionResponse ext_response;
  try {
    auto client = EXClient(extension_path);
    client.get()->call(ext_response, registry, item, request);
  } catch (const std::exception& e) {
    return Status(1, "Extension call failed: " + std::string(e.what()));
  }

  // Convert from Thrift-internal list type to PluginResponse type.
  if (ext_response.status.code == ExtensionCode::EXT_SUCCESS) {
    for (const auto& item : ext_response.response) {
      response.push_back(item);
    }
  }
  return Status(ext_response.status.code, ext_response.status.message);
}

Status startExtensionWatcher(const std::string& manager_path,
                             size_t interval,
                             bool fatal) {
  // Make sure the extension manager path exists, and is writable.
  auto status = extensionPathActive(manager_path, true);
  if (!status.ok()) {
    return status;
  }

  // Start a extension watcher, if the manager dies, so should we.
  Dispatcher::addService(
      std::make_shared<ExtensionWatcher>(manager_path, interval, fatal));
  return Status(0, "OK");
}

Status startExtensionManager() {
  if (FLAGS_disable_extensions) {
    return Status(1, "Extensions disabled");
  }
  return startExtensionManager(FLAGS_extensions_socket);
}

Status startExtensionManager(const std::string& manager_path) {
// Check if the socket location exists.
#ifdef WIN32
  auto status = isNamedPipePathValid(manager_path);
#else
  auto status = socketWritable(manager_path);
#endif

  if (!status.ok()) {
    return status;
  }

  // Seconds converted to milliseconds, used as a thread interruptible.
  auto latency = atoi(FLAGS_extensions_interval.c_str()) * 1000;
  // Start a extension manager watcher, to monitor all registered extensions.
  Dispatcher::addService(
      std::make_shared<ExtensionManagerWatcher>(manager_path, latency));

  // Start the extension manager thread.
  Dispatcher::addService(
      std::make_shared<ExtensionManagerRunner>(manager_path));

  // The shell or daemon flag configuration may require an extension.
  if (!FLAGS_extensions_require.empty()) {
    bool waited = false;
    auto extensions = osquery::split(FLAGS_extensions_require, ",");
    for (const auto& extension : extensions) {
      auto status = applyExtensionDelay(([extension, &waited](bool& stop) {
        ExtensionList extensions;
        if (getExtensions(extensions).ok()) {
          for (const auto& existing : extensions) {
            if (existing.second.name == extension) {
              return pingExtension(getExtensionSocket(existing.first));
            }
          }
        }

        if (waited) {
          // If we have already waited for the timeout period, stop early.
          stop = true;
        }
        return Status(1, "Extension not autoloaded: " + extension);
      }));

      // A required extension was not loaded.
      waited = true;
      if (!status.ok()) {
        LOG(WARNING) << status.getMessage();
        return status;
      }
    }
  }

  return Status(0, "OK");
}
}
