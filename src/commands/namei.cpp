// SPDX-License-Identifier: MIT
// Copyright (c) 2026 caomengxuan666 <caomengxuan666@users.noreply.github.com>
/// @contributors:
///   - caomengxuan666 <2507560089@qq.com>
/// @Description: Implementation for namei (display path resolution steps).
/// @Version: 0.1.0
/// @License: MIT
/// @Copyright: Copyright © 2026 WinuxCmd

#include "core/command_macros.h"
#include "pch/pch.h"

import std;
import core;
import utils;
import container;

using cmd::meta::OptionMeta;
using cmd::meta::OptionType;

auto constexpr NAMEI_OPTIONS = std::array{
    OPTION("-a", "--access", "also print access permissions"),
    OPTION("-d", "--noreport", "do not follow symlink if final component"),
    OPTION("-m", "", "show permissions as symbols (rwx)"),
    OPTION("-l", "--long", "long format (permissions and mode info)")};

namespace namei_pipeline {
namespace cp = core::pipeline;

struct Config {
  bool access = false;
  bool noreport = false;
  bool long_format = false;
  SmallVector<std::string, 16> paths;
};

auto build_config(const CommandContext<NAMEI_OPTIONS.size()>& ctx)
    -> cp::Result<Config> {
  Config cfg;
  cfg.access = ctx.has("-a");
  cfg.long_format = ctx.has("-l") || ctx.has("--long");
  if (ctx.has("-d")) cfg.noreport = true;
  for (auto arg : ctx.positionals) cfg.paths.push_back(std::string(arg));
  if (cfg.paths.empty()) return std::unexpected("missing operand");
  return cfg;
}

// Check if a path component is a symlink (reparse point) and return target.
auto resolve_component(const std::string& path,
                       std::optional<std::string>& target) -> bool {
  target = std::nullopt;
  std::wstring wpath = utf8_to_wstring(path);
  DWORD attrs = GetFileAttributesW(wpath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;

  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    HANDLE h = CreateFileW(
        wpath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // Query symlink target
    constexpr size_t buf_size = 4096;
    std::vector<wchar_t> buf(buf_size);
    DWORD ret = 0;
    BOOL ok = GetFinalPathNameByHandleW(
        h, buf.data(), static_cast<DWORD>(buf_size), VOLUME_NAME_DOS);
    CloseHandle(h);
    if (ok && ret > 0 && ret <= buf_size) {
      target = wstring_to_utf8(buf.data());
    }
  }
  return true;
}

// One resolution step over the folded native spelling: the cumulative
// prefix to probe, and the component tail to display.
struct WalkStep {
  std::wstring prefix;
  std::wstring name;
};

// Split the folded native spelling into incremental prefixes.
// "E:\a\b" -> "E:\", "E:\a", "E:\a\b"; relative "a\b" -> "a", "a\b";
// UNC "\\srv\share\x" -> "\\srv\share", "\\srv\share\x"; a POSIX-rooted
// spelling that no fold claimed ("/usr/bin") roots at "/".
auto native_walk_steps(std::wstring_view native) -> std::vector<WalkStep> {
  const auto is_sep = [](wchar_t ch) { return ch == L'/' || ch == L'\\'; };
  const auto is_letter = [](wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
  };
  std::vector<WalkStep> steps;
  if (native.empty()) return steps;

  size_t pos = 0;
  if (native.size() >= 2 && native[1] == L':' && is_letter(native[0])) {
    // Drive-rooted native form ("E:\..." from the fold, or the Windows
    // spelling as typed): the root step is the unambiguous "E:\", never the
    // drive-relative "E:" (which would track the per-drive current
    // directory). A bare "E:" operand stays one step.
    pos = (native.size() > 2 && is_sep(native[2])) ? 3 : 2;
    std::wstring root(native.substr(0, pos));
    steps.push_back({root, root});
  } else if (is_sep(native[0])) {
    if (native.size() >= 2 && is_sep(native[1])) {
      // UNC: "\\<server>\<share>" is the root component.
      size_t p = 2;
      while (p < native.size() && !is_sep(native[p])) ++p;
      if (p < native.size()) {
        ++p;
        while (p < native.size() && !is_sep(native[p])) ++p;
      }
      pos = p;
    } else {
      pos = 1;
    }
    std::wstring root(native.substr(0, pos));
    steps.push_back({root, root});
  }

  std::wstring prefix;
  while (pos < native.size()) {
    while (pos < native.size() && is_sep(native[pos])) ++pos;
    const size_t start = pos;
    while (pos < native.size() && !is_sep(native[pos])) ++pos;
    if (start == pos) break;
    prefix = std::wstring(native.substr(0, pos));
    steps.push_back({prefix, std::wstring(native.substr(start, pos - start))});
  }
  return steps;
}

auto run(const Config& cfg) -> int {
  for (const auto& path : cfg.paths) {
    safePrintLn("");
    safePrintLn(path);

    // #1237: the operand may use any accepted POSIX drive spelling
    // ("/e/Data", "/cygdrive/e/Data", "/mnt/e/Data").  Probing the raw
    // text's components with GetFileAttributesW resolved every one of them
    // against the CURRENT drive, so all dialect forms reported a broken
    // chain.  The operand folds once through the shared make_api_path_operand
    // boundary and the walk runs on the folded native spelling; the header
    // above still prints the operand exactly as the user typed it.
    const auto operand = native_path::make_api_path_operand(path);

    const auto steps = native_walk_steps(operand.normalized);
    for (size_t i = 0; i < steps.size(); ++i) {
      const auto& step = steps[i];
      const std::string current = wstring_to_utf8(step.prefix);
      const std::wstring& part = step.name;

      std::optional<std::string> target;
      bool is_file = resolve_component(current, target);

      if (!is_file) {
        safePrint("   ");
        for (int j = 0; j < (int)i - 1; j++) safePrint(" ");
        safePrint("f ");
        safePrint(part);
        safePrintLn(" (No such file or directory)");
        break;
      }

      safePrint("   ");
      for (int j = 0; j < (int)i - 1; j++) safePrint(" ");

      if (target.has_value()) {
        // Symlink detected
        safePrint("s ");
        safePrint(part);
        safePrintLn(" -> " + *target);
      } else {
        // Regular file/dir
        std::wstring wcur = utf8_to_wstring(current);
        DWORD attrs = GetFileAttributesW(wcur.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
          safePrint("f ");
        } else {
          safePrint("d ");
        }
        safePrint(part);
        safePrintLn("");
      }
    }
  }
  return 0;
}

}  // namespace namei_pipeline

REGISTER_COMMAND(namei, "namei", "namei [OPTION]... PATH...",
                 "Call namei on each PATH, showing any symlink resolution.\n"
                 "Displays the resolution steps for each path component.",
                 "  namei \/usr\/bin\/ls\n"
                 "  namei -l /path/to/file",
                 "readlink(1), realpath(1)", "WinuxCmd",
                 "Copyright © 2026 WinuxCmd", NAMEI_OPTIONS) {
  using namespace namei_pipeline;

  auto cfg_result = build_config(ctx);
  if (!cfg_result) {
    if (cfg_result.error() == "missing operand") {
      safeErrorPrintLn("namei: missing operand");
      safeErrorPrintLn("Try 'namei --help' for more information.");
      return 1;
    }
    cp::report_error(cfg_result, L"namei");
    return 1;
  }

  return run(*cfg_result);
}
