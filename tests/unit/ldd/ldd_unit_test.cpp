#include "framework/winuxtest.h"

TEST(ldd, lists_imports_for_winuxcmd_exe) {
  Pipeline p;
  p.add(L"ldd.exe", {std::wstring(WINUXCMD_BIN_DIR) + L"/winuxcmd.exe"});

  auto r = p.run();

  ASSERT_EQ(r.exit_code, 0);
  ASSERT_FALSE(r.stdout_text.empty());
  EXPECT_CONTAINS(r.stdout_text, ".dll");
  EXPECT_CONTAINS(r.stdout_text, "=>");
}

TEST(ldd, name_mode_prints_only_dll_names) {
  Pipeline p;
  p.add(L"ldd.exe",
        {L"--name", std::wstring(WINUXCMD_BIN_DIR) + L"/winuxcmd.exe"});

  auto r = p.run();

  ASSERT_EQ(r.exit_code, 0);
  ASSERT_FALSE(r.stdout_text.empty());
  EXPECT_CONTAINS(r.stdout_text, ".dll");
  EXPECT_NOT_CONTAINS(r.stdout_text, "=>");
}

TEST(ldd, short_name_option_lists_imports) {
  Pipeline p;
  p.add(L"ldd.exe", {L"-n", std::wstring(WINUXCMD_BIN_DIR) + L"/winuxcmd.exe"});

  auto r = p.run();

  ASSERT_EQ(r.exit_code, 0);
  ASSERT_FALSE(r.stdout_text.empty());
  EXPECT_CONTAINS(r.stdout_text, ".dll");
  EXPECT_NOT_CONTAINS(r.stdout_text, "=>");
}

TEST(ldd, unused_option_lists_direct_dependencies) {
  Pipeline p;
  p.add(L"ldd.exe",
        {L"--unused", std::wstring(WINUXCMD_BIN_DIR) + L"/winuxcmd.exe"});

  auto r = p.run();

  ASSERT_EQ(r.exit_code, 0);
  ASSERT_FALSE(r.stdout_text.empty());
  EXPECT_CONTAINS(r.stdout_text, ".dll");
  EXPECT_NOT_CONTAINS(r.stdout_text, "=>");
}

TEST(ldd, verbose_option_prints_version_heading) {
  Pipeline p;
  p.add(L"ldd.exe",
        {L"--verbose", std::wstring(WINUXCMD_BIN_DIR) + L"/winuxcmd.exe"});

  auto r = p.run();

  ASSERT_EQ(r.exit_code, 0);
  ASSERT_FALSE(r.stdout_text.empty());
  EXPECT_CONTAINS(r.stdout_text, "Version information:");
  EXPECT_CONTAINS(r.stdout_text, ".dll");
}

TEST(ldd, rejects_non_pe_file) {
  TempDir tmp;
  auto file = tmp.write_file("plain.txt", "not pe");

  Pipeline p;
  p.add(L"ldd.exe", {file.wstring()});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 1);
  EXPECT_CONTAINS(r.stderr_text, "not a PE executable");
}

// ===== #1238 regression: POSIX drive-dialect operands =====
//
// `ldd /e/x.exe` used to hand the raw dialect text to std::ifstream, and the
// CRT resolves a leading "/e" against the CURRENT drive - from a foreign cwd
// every POSIX spelling reported "cannot open" for a file that exists on the
// operand's drive (MSYS ldd opens the same spelling from anywhere). The
// probes below synthesize the dialect spellings from a real PE's own drive,
// so the assertions work on any host regardless of which drives exist.
namespace ldd_dialect_detail {

// The MSYS single-letter drive spellings of a native path
// ("D:\repo\x.exe"): "/d/repo/x.exe", "/cygdrive/d/repo/x.exe",
// "/mnt/d/repo/x.exe". Both dialect prefixes fold to the MSYS spelling
// before the drive branch (#1145, #1172), so all three must resolve.
auto slash_drive_form(const std::filesystem::path &native) -> std::wstring {
  const std::wstring text = native.wstring();
  wchar_t drive = text[0];
  if (drive >= L'A' && drive <= L'Z') {
    drive = static_cast<wchar_t>(drive - L'A' + L'a');
  }
  std::wstring dialect;
  dialect.push_back(L'/');
  dialect.push_back(drive);
  for (size_t i = 2; i < text.size(); ++i) {
    dialect.push_back(text[i] == L'\\' ? L'/' : text[i]);
  }
  return dialect;
}

auto cygdrive_form(const std::filesystem::path &native) -> std::wstring {
  return L"/cygdrive" + slash_drive_form(native);
}

auto mnt_form(const std::filesystem::path &native) -> std::wstring {
  return L"/mnt" + slash_drive_form(native);
}

// First fixed drive whose letter differs from OTHER, or L'\0' when none.
auto other_fixed_drive(wchar_t other) -> wchar_t {
  wchar_t buf[512];
  const DWORD len =
      GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buf)), buf);
  for (DWORD i = 0; i + 1 < len;) {
    const wchar_t *entry = buf + i;
    const size_t entry_len = std::wcslen(entry);
    if (entry_len == 0) break;
    i += static_cast<DWORD>(entry_len) + 1;
    if (entry_len < 3 || entry[1] != L':' || entry[2] != L'\\') continue;
    wchar_t drive = entry[0];
    if (drive >= L'A' && drive <= L'Z') {
      drive = static_cast<wchar_t>(drive - L'A' + L'a');
    }
    if (drive == other) continue;
    if (GetDriveTypeW(entry) != DRIVE_FIXED) continue;
    return drive;
  }
  return L'\0';
}

void clear_shell_root_env(Pipeline &pipeline) {
  for (const auto *name :
       {L"__RUBASH_SHELL_ROOT", L"NIU_ROOT", L"WINUXSH_ROOT", L"RUBASH_ROOT"}) {
    pipeline.set_env(name, L"");
  }
}

}  // namespace ldd_dialect_detail

TEST(ldd, drive_dialect_spellings_resolve_real_pe_from_foreign_cwd) {
  // The build tree's own winuxcmd.exe is the fixture PE: dialect-spell its
  // native location instead of copying megabytes into a TempDir.
  const std::filesystem::path pe =
      ProjectPaths::detect_bin_dir() / "winuxcmd.exe";
  ASSERT_TRUE(std::filesystem::exists(pe));

  TempDir cwd;
  for (const auto &operand : {ldd_dialect_detail::slash_drive_form(pe),
                              ldd_dialect_detail::cygdrive_form(pe),
                              ldd_dialect_detail::mnt_form(pe)}) {
    Pipeline p;
    p.set_cwd(cwd.wpath());
    ldd_dialect_detail::clear_shell_root_env(p);
    p.add(L"ldd.exe", {operand});

    auto r = p.run();

    TEST_LOG_CMD_LIST("ldd.exe", operand);
    TEST_LOG_EXIT_CODE(r);
    TEST_LOG("ldd dialect stdout", r.stdout_text);
    TEST_LOG("ldd dialect stderr", r.stderr_text);

    // Before #1238 the raw dialect text stranded on the cwd drive and the
    // open failed even though the PE exists.  MSYS ldd opens the same
    // spelling from any cwd.
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_CONTAINS(r.stdout_text, ".dll");
    EXPECT_CONTAINS(r.stdout_text, "=>");
  }
}

TEST(ldd, drive_dialect_cwd_on_other_drive_resolves_non_pe_probe) {
  TempDir tmp;
  tmp.write("probe1238.txt", "not pe");
  const std::wstring operand =
      ldd_dialect_detail::slash_drive_form(tmp.path / L"probe1238.txt");
  const wchar_t tmp_drive = tmp.path.wstring()[0];
  const wchar_t other = ldd_dialect_detail::other_fixed_drive(
      tmp_drive >= L'A' && tmp_drive <= L'Z'
          ? static_cast<wchar_t>(tmp_drive - L'A' + L'a')
          : tmp_drive);
  if (other == L'\0') {
    std::cout << "  SKIPPED (no second fixed drive to host the cwd)\n";
    return;
  }

  Pipeline p;
  p.set_cwd(std::wstring(1, other) + L":\\");
  ldd_dialect_detail::clear_shell_root_env(p);
  p.add(L"ldd.exe", {operand});

  auto r = p.run();

  TEST_LOG_CMD_LIST("ldd.exe", operand);
  TEST_LOG_EXIT_CODE(r);
  TEST_LOG("ldd dialect stderr", r.stderr_text);

  // The probe file only exists at the dialect location, so "not a PE
  // executable" proves the fold landed the open on the operand's drive;
  // the pre-fix code stranded the spelling on the cwd drive and reported
  // "cannot open" instead.  The message keeps the operand as typed
  // (MSYS quotes the spelling too).
  EXPECT_EQ(r.exit_code, 1);
  EXPECT_CONTAINS(r.stderr_text, "not a PE executable");
  EXPECT_NOT_CONTAINS(r.stderr_text, "cannot open");
}
