#include "framework/winuxtest.h"

TEST(mkfifo, mkfifo_missing_operand) {
  Pipeline p;
  p.add(L"mkfifo.exe", {});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 1);
  EXPECT_NE(r.stderr_text.find("missing operand"), std::string::npos);
}

TEST(mkfifo, mkfifo_invalid_mode_fails) {
  Pipeline p;
  p.add(L"mkfifo.exe", {L"-m", L"bad!mode", L"pipe1"});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 1);
  EXPECT_NE(r.stderr_text.find("invalid mode"), std::string::npos);
}

TEST(mkfifo, mkfifo_existing_path_fails_like_gnu_shape) {
  TempDir tmp;
  tmp.write("pipe1", "already here");

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {L"pipe1"});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 1);
  EXPECT_NE(r.stderr_text.find("File exists"), std::string::npos);
}

TEST(mkfifo, mkfifo_creates_fifo_marker) {
  TempDir tmp;

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {L"pipe1"});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(tmp.read("pipe1"), "!<fifo>");
  EXPECT_NE(tmp.attrs("pipe1") & FILE_ATTRIBUTE_SYSTEM, 0u);
}

TEST(mkfifo, mkfifo_marker_is_seen_as_fifo) {
  TempDir tmp;

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {L"pipe1"});
  ASSERT_EQ(p.run().exit_code, 0);

  Pipeline t;
  t.set_cwd(tmp.wpath());
  t.add(L"test.exe", {L"-p", L"pipe1"});
  EXPECT_EQ(t.run().exit_code, 0);

  Pipeline tf;
  tf.set_cwd(tmp.wpath());
  tf.add(L"test.exe", {L"-f", L"pipe1"});
  EXPECT_EQ(tf.run().exit_code, 1);

  Pipeline s;
  s.set_cwd(tmp.wpath());
  s.add(L"stat.exe", {L"-c", L"%F", L"pipe1"});
  auto sr = s.run();
  EXPECT_EQ(sr.exit_code, 0);
  EXPECT_NE(sr.stdout_text.find("fifo"), std::string::npos);
}

TEST(mkfifo, mkfifo_multiple_operands_create_each) {
  TempDir tmp;

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {L"pipe1", L"pipe2"});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(tmp.read("pipe1"), "!<fifo>");
  EXPECT_EQ(tmp.read("pipe2"), "!<fifo>");
}

TEST(mkfifo, mkfifo_mode_umask_applies_readonly) {
  TempDir tmp;

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {L"-m", L"444", L"pipe1"});

  auto r = p.run();

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_NE(tmp.attrs("pipe1") & FILE_ATTRIBUTE_READONLY, 0u);
}

// ===== #1235 regression: POSIX drive-dialect operands =====
// mkfifo handed the raw operand ('/e/p', '/cygdrive/e/p', '/mnt/e/p') to the
// existence probe and create_winux_fifo_w. Win32 resolves a leading "/e"
// against the CURRENT drive, so from a cwd on another drive the probe
// answered for "<cwd>:\e\p" (ENOENT) and the marker would have landed beside
// the cwd. The probes below synthesize the dialect spelling from the
// TempDir's own drive, so they pass on any host.
namespace mkfifo_dialect_detail {

// Lowercase drive letter of an absolute path ("C:\x" -> L'c').
auto drive_letter(const std::filesystem::path &path) -> wchar_t {
  wchar_t drive = path.wstring()[0];
  if (drive >= L'A' && drive <= L'Z') {
    drive = static_cast<wchar_t>(drive - L'A' + L'a');
  }
  return drive;
}

// "C:\Users\...\wct123" -> "/c/Users/.../wct123" (MSYS drive spelling).
auto slash_drive_form(const std::filesystem::path &path) -> std::wstring {
  std::wstring text = path.wstring();
  text[0] = drive_letter(path);
  text.erase(1, 1);  // drop the ':' from "c:\..." (MSYS spelling has none)
  for (wchar_t &ch : text) {
    if (ch == L'\\') ch = L'/';
  }
  return L"/" + text;
}

// First fixed drive whose letter differs from OTHER, or L'\0' when none.
auto other_fixed_drive(wchar_t other) -> wchar_t {
  const DWORD mask = GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((mask & (1u << (letter - L'A'))) == 0) continue;
    if (letter == other) continue;
    if (GetDriveTypeW((std::wstring(1, letter) + L":\\").c_str()) !=
        DRIVE_FIXED) {
      continue;
    }
    return letter;
  }
  return L'\0';
}

// Where the BUGGY code put the probe leaf instead: rooted on the cwd drive,
// "<cwd>:\e\<tmp-rest>\probe1235".
auto shadow_leaf(const TempDir &tmp, wchar_t cwd_drive, const wchar_t *leaf)
    -> std::filesystem::path {
  const wchar_t operand_drive = drive_letter(tmp.path);
  std::wstring text(1, cwd_drive);
  text += L":\\";
  text += operand_drive;
  text += L"\\";
  text += tmp.path.wstring().substr(3);  // past "X:\"
  return std::filesystem::path(text) / leaf;
}

}  // namespace mkfifo_dialect_detail

TEST(mkfifo, mkfifo_drive_dialect_operand_lands_on_operand_drive) {
  TempDir tmp;
  const wchar_t tmp_drive = mkfifo_dialect_detail::drive_letter(tmp.path);
  const std::wstring operand =
      mkfifo_dialect_detail::slash_drive_form(tmp.path / L"probe1235");

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {operand});

  auto r = p.run();

  TEST_LOG_CMD_LIST("mkfifo.exe", operand);
  TEST_LOG_EXIT_CODE(r);
  TEST_LOG("mkfifo stderr", r.stderr_text);

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_TRUE(r.stderr_text.empty());
  // Landing: the marker file under the TempDir, on the operand's drive.
  EXPECT_EQ(tmp.read("probe1235"), "!<fifo>");
  EXPECT_NE(tmp.attrs("probe1235") & FILE_ATTRIBUTE_SYSTEM, 0u);
  // Shadow negative (#1235): the unfolded operand rooted the leaf on the cwd
  // drive ("<cwd>:\e\<tmp-rest>") - that leaf must never exist.
  EXPECT_FALSE(std::filesystem::exists(
      mkfifo_dialect_detail::shadow_leaf(tmp, tmp_drive, L"probe1235")));
}

TEST(mkfifo, mkfifo_drive_dialect_cwd_on_other_drive_lands_on_operand_drive) {
  TempDir tmp;
  const wchar_t other = mkfifo_dialect_detail::other_fixed_drive(
      mkfifo_dialect_detail::drive_letter(tmp.path));
  if (other == L'\0') {
    std::cout << "  SKIPPED (no second fixed drive to host the cwd)\n";
    return;
  }
  const std::wstring operand =
      mkfifo_dialect_detail::slash_drive_form(tmp.path / L"probe1235");

  Pipeline p;
  p.set_cwd(std::wstring(1, other) + L":\\");
  p.add(L"mkfifo.exe", {operand});

  auto r = p.run();

  TEST_LOG_CMD_LIST("mkfifo.exe", operand);
  TEST_LOG_EXIT_CODE(r);
  TEST_LOG("mkfifo stderr", r.stderr_text);

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_TRUE(r.stderr_text.empty());
  EXPECT_EQ(tmp.read("probe1235"), "!<fifo>");
  // The audit's exact shadow shape: cwd on one drive, operand on another -
  // the unfolded spelling wrote "<cwd>:\e\..." on the cwd drive.
  EXPECT_FALSE(std::filesystem::exists(
      mkfifo_dialect_detail::shadow_leaf(tmp, other, L"probe1235")));
}

TEST(mkfifo, mkfifo_drive_dialect_cygdrive_and_mnt_prefixes_lands) {
  TempDir tmp;
  const std::wstring slash =
      mkfifo_dialect_detail::slash_drive_form(tmp.path / L"probe1235");
  // "/cygdrive/c/..." and "/mnt/c/..." fold onto the same file (#1145/#1172).
  const std::wstring cyg = L"/cygdrive" + slash;
  const std::wstring mnt = L"/mnt" + slash;

  {
    Pipeline p;
    p.set_cwd(tmp.wpath());
    p.add(L"mkfifo.exe", {cyg});
    auto r = p.run();
    TEST_LOG_CMD_LIST("mkfifo.exe", cyg);
    TEST_LOG_EXIT_CODE(r);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(r.stderr_text.empty());
    EXPECT_EQ(tmp.read("probe1235"), "!<fifo>");
  }
  std::filesystem::remove(tmp.path / L"probe1235");
  {
    Pipeline p;
    p.set_cwd(tmp.wpath());
    p.add(L"mkfifo.exe", {mnt});
    auto r = p.run();
    TEST_LOG_CMD_LIST("mkfifo.exe", mnt);
    TEST_LOG_EXIT_CODE(r);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(r.stderr_text.empty());
    EXPECT_EQ(tmp.read("probe1235"), "!<fifo>");
  }
}

TEST(mkfifo, mkfifo_bare_drive_root_operand_reports_file_exists) {
  TempDir tmp;
  const wchar_t drive = mkfifo_dialect_detail::drive_letter(tmp.path);
  const std::wstring operand = std::wstring(L"/") + drive;
  // What the buggy code did here: created a marker FILE at "<drive>:\<drive>"
  // when the cwd sat elsewhere. Only assert absence when it does not
  // pre-exist.
  const std::filesystem::path shadow_root =
      std::filesystem::path(std::wstring(1, drive) + L":\\" + drive);
  const bool shadow_pre_existed = std::filesystem::exists(shadow_root);

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {operand});

  auto r = p.run();

  TEST_LOG_CMD_LIST("mkfifo.exe", operand);
  TEST_LOG_EXIT_CODE(r);
  TEST_LOG("mkfifo stderr", r.stderr_text);

  EXPECT_EQ(r.exit_code, 1);
  EXPECT_NE(r.stderr_text.find("File exists"), std::string::npos);
  if (!shadow_pre_existed) {
    EXPECT_FALSE(std::filesystem::exists(shadow_root));
  }
}

TEST(mkfifo, mkfifo_drive_dialect_existing_target_quotes_operand_spelling) {
  TempDir tmp;
  tmp.write("pipe1235", "already here");
  const std::wstring operand =
      mkfifo_dialect_detail::slash_drive_form(tmp.path / L"pipe1235");
  const std::string operand_utf8(operand.begin(), operand.end());

  Pipeline p;
  p.set_cwd(tmp.wpath());
  p.add(L"mkfifo.exe", {operand});

  auto r = p.run();

  TEST_LOG_CMD_LIST("mkfifo.exe", operand);
  TEST_LOG_EXIT_CODE(r);

  EXPECT_EQ(r.exit_code, 1);
  // The message quotes the spelling the user handed over, never the folded
  // "X:\..." API form (#1243 display precedent).
  EXPECT_NE(r.stderr_text.find("cannot create fifo '" + operand_utf8 + "'"),
            std::string::npos);
  EXPECT_EQ(r.stderr_text.find("X:\\"), std::string::npos);
}
