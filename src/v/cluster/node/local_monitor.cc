/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "cluster/node/local_monitor.h"

#include "cluster/logger.h"
#include "cluster/node/types.h"
#include "config/configuration.h"
#include "config/node_config.h"
#include "utils/human.h"
#include "version.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sstring.hh>

#include <fmt/core.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <seastarx.h>

namespace cluster::node {

ss::future<> local_monitor::update_state() {
    refresh_configuration();

    // grab new snapshot of local state
    auto disks = co_await get_disks();
    auto vers = application_version(ss::sstring(redpanda_version()));
    auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
      ss::engine().uptime());

    _state = {
      .redpanda_version = vers,
      .uptime = uptime,
      .disks = disks,
    };
    update_alert_state();
    co_return;
}

const local_state& local_monitor::get_state_cached() const { return _state; }

void local_monitor::set_path_for_test(const ss::sstring& path) {
    _path_for_test = path;
}

void local_monitor::set_statvfs_for_test(
  std::function<struct statvfs(const ss::sstring&)> func) {
    _statvfs_for_test = std::move(func);
}

std::tuple<size_t, size_t>
local_monitor::minimum_free_by_bytes_and_percent(size_t bytes_available) const {
    long double percent_factor = last_free_space_percent_threshold / 100.0;
    return std::make_tuple(
      last_free_space_bytes_threshold, percent_factor * bytes_available);
}

ss::future<std::vector<disk>> local_monitor::get_disks() {
    auto path = _path_for_test.empty()
                  ? config::node().data_directory().as_sstring()
                  : _path_for_test;

    auto svfs = co_await get_statvfs(path);

    co_return std::vector<disk>{disk{
      .path = config::node().data_directory().as_sstring(),
      // f_bsize is a historical linux-ism, use f_frsize
      .free = svfs.f_bfree * svfs.f_frsize,
      .total = svfs.f_blocks * svfs.f_frsize,
    }};
}

ss::future<struct statvfs> local_monitor::get_statvfs(const ss::sstring& path) {
    if (_statvfs_for_test) {
        co_return _statvfs_for_test.value()(path);
    } else {
        co_return co_await ss::engine().statvfs(path);
    }
}

float percent_free(const disk& disk) {
    long double free = disk.free, total = disk.total;
    return float((free / total) * 100.0);
}

void local_monitor::maybe_log_space_error(const disk& disk) {
    auto [min_by_bytes, min_by_percent] = minimum_free_by_bytes_and_percent(
      disk.total);
    auto min_space = std::min(min_by_percent, min_by_bytes);
    clusterlog.log(
      ss::log_level::error,
      despam_interval,
      "{}: free space at {:.3f}% on {}: {} total, {} free, "
      "min. free {}. Please adjust retention policies as needed to "
      "avoid running out of space.",
      stable_alert_string,
      percent_free(disk),
      disk.path,
      // TODO: generalize human::bytes for unsigned long
      human::bytes(disk.total), // NOLINT narrowing conv.
      human::bytes(disk.free),  // NOLINT  "  "
      human::bytes(min_space)); // NOLINT  "  "
}

void local_monitor::update_alert_state() {
    _state.storage_space_alert = disk_space_alert::ok;
    for (const auto& d : _state.disks) {
        vassert(d.total != 0.0, "Total disk space cannot be zero.");
        auto [min_by_bytes, min_by_percent] = minimum_free_by_bytes_and_percent(
          d.total);
        auto min_space = std::min(min_by_percent, min_by_bytes);
        clusterlog.debug(
          "min by % {}, min bytes {}, disk.free {} -> alert {}",
          min_by_percent,
          min_by_bytes,
          d.free,
          d.free <= min_space);

        if (unlikely(d.free <= min_space)) {
            _state.storage_space_alert = disk_space_alert::low_space;
            maybe_log_space_error(d);
        }
    }
}

void local_monitor::refresh_configuration() {
    auto percent_threshold = get_config_alert_threshold_percent();
    auto bytes_threshold = get_config_alert_threshold_bytes();
    if (last_free_space_percent_threshold != percent_threshold) {
        clusterlog.info(
          "Updated free space percent alert threshold {} -> {}",
          last_free_space_percent_threshold,
          percent_threshold);
        last_free_space_percent_threshold = percent_threshold;
    }

    if (last_free_space_bytes_threshold != bytes_threshold) {
        clusterlog.info(
          "Updated free space bytes alert threshold {} -> {}",
          last_free_space_bytes_threshold,
          bytes_threshold);
        last_free_space_bytes_threshold = bytes_threshold;
    }
}

std::size_t local_monitor::get_config_alert_threshold_bytes() {
    return config::shard_local_cfg().storage_space_alert_free_threshold_bytes();
}

unsigned local_monitor::get_config_alert_threshold_percent() {
    return config::shard_local_cfg()
      .storage_space_alert_free_threshold_percent();
}

} // namespace cluster::node