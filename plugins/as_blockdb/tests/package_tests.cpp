// as_blockdb package regression — the public zip must never contain live secrets.
//
// package.sh used to read the first existing BlockDB.ini from ref/pisex/live-configs-admin_system
// (or the module repo) and copy it into the package. A real live file can carry an ApiKey and a
// Discord webhook token, and the package is published as a public zip; the script was also unable
// to run without ref/.
//
// This test runs the real package.sh from an isolated copy of the plugin directory (no ref/ at
// all), with temp BUILD_DIR and OUT_DIR and fake build artifacts:
//   * the package succeeds and ships plugins/as_blockdb/configs/admin_system/BlockDB.ini
//     byte-identically, with an EMPTY ApiKey and webhook;
//   * no live/ref file is read - the temp tree has no ref/, and the script source has no
//     ref/pisex or live-configs reference;
//   * an existing server config is never modified;
//   * OUT_DIR is refused unless it is a strict subdirectory of BUILD_DIR (outside dir, BUILD_DIR
//     itself, `/`, a symlink escape and a server-root hook are all rejected *before* rm -rf);
//   * missing build artifacts fail cleanly and leave no output.
//
// No engine, no bindings, no network. Runs from build/tests via ./test.sh.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s\n", what.c_str());
	}
}

std::string Quote(const std::string& text) {
	std::string quoted = "'";
	for (const char c : text) {
		if (c == '\'') {
			quoted += "'\\''";
		} else {
			quoted += c;
		}
	}
	quoted += "'";
	return quoted;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << content;
}

void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
	std::filesystem::create_directories(to.parent_path());
	std::error_code error;
	std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
	if (error) {
		std::printf("FAIL: cannot copy %s -> %s: %s\n", from.c_str(), to.c_str(), error.message().c_str());
		++g_failures;
	}
}

int RunPackage(const std::filesystem::path& script, const std::filesystem::path& buildDir,
			   const std::filesystem::path& outDir, const std::filesystem::path& logFile,
			   const std::string& envPrefix = {}) {
	std::string command = envPrefix;
	command += "BUILD_DIR=" + Quote(buildDir.string()) + " OUT_DIR=" + Quote(outDir.string()) + " /bin/bash " +
			   Quote(script.string()) + " >" + Quote(logFile.string()) + " 2>&1";
	const int status = std::system(command.c_str());
	if (status == -1) {
		return -1;
	}
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return 128;
}

void PrintLogOnFailure(int exitCode, const std::filesystem::path& logFile, std::string_view what) {
	if (exitCode == 0) {
		return;
	}
	std::printf("package.sh failed for %s (exit %d):\n", std::string(what).c_str(), exitCode);
	if (const auto log = ReadFile(logFile)) {
		std::printf("%s\n", log->c_str());
	}
}

void TestPackageLayout(const std::filesystem::path& outDir, const std::filesystem::path& sourceConfig) {
	for (const char* relative : {"as_blockdb.pplugin", "bin/libas_blockdb.so",
								 "configs/admin_system/BlockDB.ini"}) {
		Check(std::filesystem::is_regular_file(outDir / relative),
			  std::string("the package contains ") + relative);
	}

	const auto packaged = ReadFile(outDir / "configs/admin_system/BlockDB.ini");
	const auto shipped = ReadFile(sourceConfig);
	Check(packaged.has_value(), "the package ships configs/admin_system/BlockDB.ini");
	Check(shipped.has_value(), "the plugin ships configs/admin_system/BlockDB.ini");
	if (!packaged || !shipped) {
		return;
	}
	Check(*packaged == *shipped,
		  "the packaged config is byte-identical to the checked-in plugin-local sample");

	as_blockdb::Config config;
	Check(as_blockdb::LoadConfigFromFile((outDir / "configs/admin_system/BlockDB.ini").string(), config),
		  "the packaged config parses with the plugin's own KV1 reader");
	Check(config.apiKey.empty(), "the packaged ApiKey is empty (no secret from a live config)");
	Check(config.webhook.empty(), "the packaged webhook is empty (no Discord token)");
	Check(config.serverName == "MIRAGE #1", "the packaged sample keeps the original server_name");
	Check(config.autoBlockBanCount == 3, "the packaged sample keeps AutoBlockBanCount 3");
}

void TestScriptSource(const std::filesystem::path& sourceDir) {
	const auto text = ReadFile(sourceDir / "package.sh");
	Check(text.has_value(), "plugins/as_blockdb/package.sh is readable");
	if (!text) {
		return;
	}
	Check(text->find("ref/pisex") == std::string::npos &&
			  text->find("live-configs") == std::string::npos &&
			  text->find("ROOT/ref") == std::string::npos,
		  "package.sh never reads a live/ref BlockDB.ini");
	Check(text->find("configs/admin_system/BlockDB.ini") != std::string::npos,
		  "package.sh ships the plugin-local configs/admin_system/BlockDB.ini");
	Check(text->find("cp -n") != std::string::npos || text->find("cp -rn") != std::string::npos,
		  "package.sh documents the no-clobber deploy rule for an existing server config");
	Check(text->find("os.path.realpath") != std::string::npos,
		  "the python fallback of absolute() resolves symlinks");
	Check(text->find("os.path.abspath") == std::string::npos,
		  "package.sh does not use the symlink-blind os.path.abspath");
	Check(text->find("AS_BLOCKDB_SERVER_ROOTS") != std::string::npos,
		  "package.sh has the server-root denial list the tests extend");
	Check(text->find("BUILD_DIR must not be the filesystem root") != std::string::npos &&
			  text->find("OUT_DIR must not be the filesystem root") != std::string::npos,
		  "package.sh refuses / as BUILD_DIR and as OUT_DIR");
}

} // namespace

int main() {
	std::printf("as_blockdb package tests (isolated, no ref/live configs)\n");

	const std::filesystem::path sourceDir = AS_BLOCKDB_TEST_SOURCE_DIR;
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("as_blockdb_package_tests_" + std::to_string(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(root, error);

	// An isolated plugin tree: only package.sh and the checked-in sample, with no ref/ at all.
	const std::filesystem::path repo = root / "repo";
	const std::filesystem::path pluginDir = repo / "plugins" / "as_blockdb";
	CopyFile(sourceDir / "package.sh", pluginDir / "package.sh");
	CopyFile(sourceDir / "configs" / "admin_system" / "BlockDB.ini",
			 pluginDir / "configs" / "admin_system" / "BlockDB.ini");
	const std::filesystem::path script = pluginDir / "package.sh";
	const std::filesystem::path sourceConfig = pluginDir / "configs" / "admin_system" / "BlockDB.ini";
	Check(!std::filesystem::exists(repo / "ref"), "the isolated tree has no ref/ (nothing to fall back to)");

	const std::filesystem::path buildDir = repo / "build";
	const std::filesystem::path outDir = buildDir / "package" / "as_blockdb";
	const std::filesystem::path logFile = root / "package.log";
	WriteFile(buildDir / "plugins" / "as_blockdb" / "libas_blockdb.so", "fake shared object\n");
	WriteFile(buildDir / "plugins" / "as_blockdb" / "as_blockdb.pplugin", "{\"name\":\"as_blockdb\"}\n");

	// A pre-existing server config, as a deployment target would hold it. package.sh must never
	// touch it; it only writes OUT_DIR.
	const std::filesystem::path serverConfig = root / "server-configs" / "admin_system" / "BlockDB.ini";
	const std::string serverConfigContent =
		"\"BlockDB\"\n{\n    \"ApiKey\" \"live-secret-must-survive\"\n    \"webhook\" "
		"\"https://discord.com/api/webhooks/1/live-token\"\n}\n";
	WriteFile(serverConfig, serverConfigContent);

	// 1. The normal isolated run.
	int exitCode = RunPackage(script, buildDir, outDir, logFile);
	PrintLogOnFailure(exitCode, logFile, "the temp build/output run");
	Check(exitCode == 0, "package.sh succeeds without ref/ and with fake artifacts");
	TestPackageLayout(outDir, sourceConfig);
	TestScriptSource(sourceDir);
	Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
		  "package.sh does not touch an existing server config");

	// 2. Idempotent: the second run rebuilds the same package with the same blank secrets.
	exitCode = RunPackage(script, buildDir, outDir, logFile);
	PrintLogOnFailure(exitCode, logFile, "the second run");
	Check(exitCode == 0, "package.sh can be run twice");
	as_blockdb::Config rebuilt;
	Check(as_blockdb::LoadConfigFromFile((outDir / "configs/admin_system/BlockDB.ini").string(), rebuilt),
		  "the rebuilt packaged config parses");
	Check(rebuilt.apiKey.empty() && rebuilt.webhook.empty(), "the rebuilt package still has blank secrets");

	// 3. OUT_DIR outside BUILD_DIR is refused before rm -rf.
	const std::filesystem::path outside = root / "outside";
	WriteFile(outside / "keep.txt", "keep\n");
	exitCode = RunPackage(script, buildDir, outside, logFile);
	Check(exitCode != 0, "package.sh refuses an OUT_DIR outside BUILD_DIR");
	Check(ReadFile(outside / "keep.txt").value_or("") == "keep\n", "the outside sentinel is not deleted");
	Check(!std::filesystem::exists(outside / "bin"), "no package dirs are created outside BUILD_DIR");

	// 4. OUT_DIR == BUILD_DIR and OUT_DIR=/ are refused.
	exitCode = RunPackage(script, buildDir, buildDir, logFile);
	Check(exitCode != 0, "package.sh refuses OUT_DIR == BUILD_DIR");
	Check(std::filesystem::exists(buildDir / "plugins" / "as_blockdb" / "libas_blockdb.so"),
		  "the build artifacts survive the refused OUT_DIR");
	exitCode = RunPackage(script, buildDir, "/", logFile);
	Check(exitCode != 0, "package.sh refuses OUT_DIR=/");

	// 5. A symlink inside BUILD_DIR must not redirect the output outside it.
	const std::filesystem::path escapeLink = buildDir / "escape";
	std::filesystem::create_directory_symlink(root / "server-configs", escapeLink, error);
	Check(!error, "the symlink escape fixture is created");
	if (!error) {
		exitCode = RunPackage(script, buildDir, escapeLink / "admin_system", logFile);
		Check(exitCode != 0, "package.sh refuses an OUT_DIR that leaves BUILD_DIR through a symlink");
		Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
			  "the config behind the symlink is not deleted");
		Check(!std::filesystem::exists(root / "server-configs" / "admin_system" / "bin"),
			  "nothing is written through the symlink");
	}

	// 6. A known server tree is refused even when OUT_DIR is inside BUILD_DIR. The test extends the
	// denial list with a temp tree through AS_BLOCKDB_SERVER_ROOTS; a sentinel proves nothing was
	// deleted or written.
	const std::filesystem::path fakeServer = root / "serverlike";
	const std::filesystem::path fakeServerConfig = fakeServer / "configs" / "admin_system" / "BlockDB.ini";
	WriteFile(fakeServerConfig, serverConfigContent);
	WriteFile(fakeServer / "plugins" / "as_blockdb" / "libas_blockdb.so", "fake shared object\n");
	WriteFile(fakeServer / "plugins" / "as_blockdb" / "as_blockdb.pplugin", "{\"name\":\"as_blockdb\"}\n");
	exitCode = RunPackage(script, fakeServer, fakeServer / "configs" / "admin_system", logFile,
						  "AS_BLOCKDB_SERVER_ROOTS=" + Quote(fakeServer.string()) + " ");
	Check(exitCode != 0, "package.sh refuses an OUT_DIR inside a known server tree");
	Check(ReadFile(fakeServerConfig).value_or("") == serverConfigContent,
		  "the fake server config is not deleted");
	Check(!std::filesystem::exists(fakeServer / "configs" / "admin_system" / "bin"),
		  "nothing is written into the fake server tree");

	// 7. Missing build artifacts fail cleanly and leave no output.
	const std::filesystem::path emptyBuild = root / "empty-build";
	std::filesystem::create_directories(emptyBuild);
	const std::filesystem::path emptyOut = emptyBuild / "package" / "as_blockdb";
	exitCode = RunPackage(script, emptyBuild, emptyOut, logFile);
	Check(exitCode != 0, "package.sh fails when the build artifacts are missing");
	Check(!std::filesystem::exists(emptyOut), "a failed package run leaves no output directory");

	std::filesystem::remove_all(root, error);
	std::printf("as_blockdb package tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
