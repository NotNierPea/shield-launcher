"""
Ships an update in one command: checks everything, builds the launcher, uploads the game files, then the launcher zip.

    tools\\ship.bat                  ships at the client's version (shield-client's watermark.hpp)
    tools\\ship.bat 1.1.5            the same, and stops unless the client is 1.1.5
    tools\\ship.bat --dry-run        only the checks and what would be uploaded
    tools\\ship.bat --allow-stale    ships even if a build is older than its source
    tools\\ship.bat --keep-launcher  ships with the published launcher although its code changed
    tools\\ship.bat --prune          also deletes pieces this and the previous manifest don't use
    tools\\ship.bat --yes            doesn't ask

For a new version: set it in watermark.hpp, build the client, then ship. Nothing is uploaded before the one question.
The order is the game files, manifest.json, then the zip, so a new launcher never arrives before its files.
"""

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import publish_content as p

TOOLS = os.path.dirname(os.path.abspath(__file__))
CODE_FILES = (".cpp", ".hpp", ".h", ".c", ".cc", ".asm", ".rc", ".def", ".inc")
TOLERANCE = 2  # seconds, a build can stamp its output in the same moment as its last source


def version_tuple(text):
    return tuple(int(part) for part in re.findall(r"\d+", text))


def walk(folder):
    for path, dirs, names in os.walk(folder):
        dirs.sort()
        for name in sorted(names):
            yield os.path.join(path, name)


def short(path, top):
    return os.path.relpath(path, top).replace("\\", "/")


def newer_than(files, stamp):
    return [file for file in files if os.path.getmtime(file) > stamp + TOLERANCE]


def same_bytes(a, b):
    with open(a, "rb") as left, open(b, "rb") as right:
        return left.read() == right.read()


def game_running():
    result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq BlackOps4.exe", "/NH"], capture_output=True, text=True)
    return "blackops4.exe" in result.stdout.lower()


def build_launcher():
    """the Release launcher and shortcut, MSBuild only rebuilds what changed"""
    installer = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    found = subprocess.run([installer, "-latest", "-prerelease", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
                            "-find", r"MSBuild\**\Bin\MSBuild.exe"], capture_output=True, text=True)
    msbuild = found.stdout.strip().splitlines()[0] if found.stdout.strip() else ""
    if not msbuild:
        p.fail("MSBuild not found")
    if subprocess.run([os.path.join(TOOLS, "premake5.exe"), "vs2026"], cwd=p.ROOT, capture_output=True).returncode != 0:
        p.fail("premake failed")
    result = subprocess.run([msbuild, os.path.join("build", "Shield_launcher.slnx"), "/p:Configuration=Release", "/p:Platform=x64",
                             "/m", "/v:minimal", "/nologo"], cwd=p.ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        errors = [line for line in result.stdout.splitlines() if " error " in line]
        p.fail("the launcher didn't build:\n" + "\n".join(errors[:8]))


class Release:
    def __init__(self, config_path):
        self.config_path = os.path.abspath(config_path)
        with open(self.config_path, encoding="utf-8") as file:
            self.config = json.load(file)
        self.base = os.path.dirname(self.config_path)
        self.vars = self.config.get("vars", {})
        self.ship = self.config["ship"]
        self.repo = self.config["repo"]
        self.tag = self.config["tag"]
        self.release_tag = self.ship["release_tag"]

    def path(self, text):
        return p.expand(text, self.vars, self.base)

    def client_dll(self):
        target = self.config["client_dll"]
        for component in self.config["components"]:
            for entry in component["files"]:
                if entry["to"].strip("/") == target:
                    return self.path(entry["from"])
        p.fail("client_dll isn't in content.json: " + target)

    def client_version(self):
        path = self.path(self.config["client_version_file"])
        if not os.path.isfile(path):
            p.fail("client version file not found: " + path)
        with open(path, encoding="utf-8") as file:
            found = re.search(r'version\s*=\s*"([^"]+)"', file.read())
        if not found:
            p.fail("no version in " + path)
        return found.group(1)

    def zip_name(self, version):
        return self.ship["zip_name"].format(version=version)


def check_client(release, version):
    """the DLL is this version and is built from the current client source"""
    problems, stale, warnings = [], [], []
    dll = release.client_dll()
    if not os.path.isfile(dll):
        return ["client DLL not found: " + dll], stale, warnings

    with open(dll, "rb") as file:
        data = file.read()
    if b"\x00" + version.encode() + b"\x00" not in data:
        built = sorted({m.group(1).decode() for m in re.finditer(rb"\x00(\d+\.\d+\.\d+)\x00", data)})
        problems.append("the client DLL isn't %s (it has %s): build the client" % (version, ", ".join(built) or "no version"))

    source = release.path(release.ship["client_source"])
    code = [file for file in walk(source) if file.lower().endswith(CODE_FILES)]
    changed = newer_than(code, os.path.getmtime(dll))
    if changed:
        newest = max(changed, key=os.path.getmtime)
        stale.append("client source is newer than the DLL (%d files, like %s): build the client" % (len(changed), short(newest, source)))

    status = subprocess.run(["git", "-C", source, "status", "--porcelain", "--", "."], capture_output=True, text=True)
    if status.returncode == 0 and status.stdout.strip():
        warnings.append("shield-client has %d uncommitted source changes" % len(status.stdout.strip().splitlines()))
    return problems, stale, warnings


def compiles_to(repo, source, compiled):
    """git gives sources new dates: a newer one is only stale if it compiles to different bytes"""
    compiler = os.path.join(repo, "hksc.exe")
    if not os.path.isfile(compiler):
        return False
    output = os.path.join(p.WORK, "lua_check.luac")
    if os.path.exists(output):
        os.remove(output)
    result = subprocess.run([compiler, os.path.relpath(source, repo), "-g", "--file-prefix-map=%s\\=" % os.path.abspath(repo), "-o", output],
                            cwd=repo, capture_output=True)
    return result.returncode == 0 and os.path.isfile(output) and same_bytes(output, compiled)


def check_support(release):
    """the shipped Lua, scripts, fastfile and config are built from the current support mod source"""
    problems, stale = [], []
    ship = release.ship
    repo = release.path(ship["support_repo"])
    mod = release.path(ship["support_mod"])
    ff = release.path(ship["support_ff"])
    config = release.path(ship["support_config"])
    for needed in (mod, ff, config):
        if not os.path.exists(needed):
            problems.append("support mod file not found: " + needed)
    if problems:
        return problems, stale

    # Lua: each source has its .luac
    late = []
    for side, output in (("Frontend Side", "Frontend"), ("In-Game Side", "Game")):
        for file in walk(os.path.join(repo, "Lua", side)):
            if not file.lower().endswith(".lua"):
                continue
            compiled = os.path.join(mod, "Lua", output, os.path.splitext(os.path.basename(file))[0] + ".luac")
            if not os.path.isfile(compiled) or (os.path.getmtime(file) > os.path.getmtime(compiled) + TOLERANCE and not compiles_to(repo, file, compiled)):
                late.append(short(file, repo))
    if late:
        stale.append("%d Lua files aren't compiled into internals (like %s)" % (len(late), late[0]))

    # GSC/CSC: all compile into the two script files
    scripts = [file for file in walk(os.path.join(repo, "Scripts")) if file.lower().endswith((".gsc", ".csc"))]
    outputs = [os.path.join(mod, "Scripts", name) for name in ("SupportGSC.gsic", "SupportCSC.csic")]
    if not all(os.path.isfile(file) for file in outputs):
        stale.append("the compiled support scripts are missing from internals")
    else:
        changed = newer_than(scripts, min(os.path.getmtime(file) for file in outputs))
        if changed:
            stale.append("%d scripts are newer than the compiled ones (like %s)" % (len(changed), short(max(changed, key=os.path.getmtime), repo)))

    # the fastfile's sources and the config it goes with
    ff_source = os.path.join(repo, "ShieldConfig", "project-bo4", "fastfile", "support_ff")
    changed = newer_than(walk(ff_source), os.path.getmtime(ff))
    if changed:
        stale.append("%d fastfile sources are newer than support.ff (like %s)" % (len(changed), short(max(changed, key=os.path.getmtime), repo)))
    repo_config = os.path.join(repo, "ShieldConfig", "project-bo4", "fastfile", "T8ShieldSupport_FF", "config.json")
    if os.path.isfile(repo_config) and not same_bytes(repo_config, config):
        stale.append("the game's T8ShieldSupport_FF/config.json differs from the support mod's")

    # plain mod files (metadata, data) are copied, not compiled
    repo_mod = os.path.join(repo, "ShieldConfig", "project-bo4", "mods", "T8ShieldSupport")
    if os.path.isdir(repo_mod):
        differ = []
        for file in walk(repo_mod):
            relative = short(file, repo_mod)
            if relative.startswith(("Lua/", "Scripts/")) or fnmatch.fnmatch(relative.lower(), "supportutils/supporthashes.txt"):
                continue
            target = os.path.join(mod, relative)
            if not os.path.isfile(target) or not same_bytes(file, target):
                differ.append(relative)
        if differ:
            stale.append("%d mod files differ from internals (like %s)" % (len(differ), differ[0]))

    # the compile scripts write to mods/, which players don't get
    dev = release.path(ship["support_dev_mod"])
    if os.path.isdir(dev):
        late = []
        for file in walk(dev):
            target = os.path.join(mod, short(file, dev))
            if not os.path.isfile(target) or os.path.getmtime(file) > os.path.getmtime(target) + TOLERANCE:
                late.append(short(file, dev))
        if late:
            stale.append("%d files in mods/T8ShieldSupport are newer than internals, which is what ships (like %s)" % (len(late), late[0]))
    return problems, stale


def launcher_plan(release, version, keep, assets):
    """"new" ships the built exe, "reuse" keeps the published one; players only update to a higher number"""
    live = p.live_manifest(release.repo, release.tag)
    entry = live.get("launcher") if live else None
    if not entry:
        return "new", []
    if version_tuple(version) < version_tuple(entry["version"]):
        return "new", ["%s is lower than the published launcher %s: players would never get it" % (version, entry["version"])]
    if version_tuple(version) > version_tuple(entry["version"]):
        return "new", []

    recorded = entry.get("source")
    if recorded and recorded != p.launcher_source_hash() and not keep:
        next_version = version.rsplit(".", 1)
        return "reuse", ["the launcher's code changed but it's still %s, so players wouldn't get it. Bump the client and run: tools\\ship.bat %s.%d (or --keep-launcher to leave it out)"
                         % (version, next_version[0], int(next_version[1]) + 1)]
    parts = {part["sha256"] + ".bin": part["size"] for part in entry["file"]["parts"] if part["size"]}
    if assets is None or any(assets.get(name) != size for name, size in parts.items()):
        return "new", []
    return "reuse", []


def write_zip(release, version):
    name = release.zip_name(version)
    path = os.path.join(p.WORK, name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for source, target, _ in p.collect(release.config["base_zip"], release.vars, release.base):
            archive.write(source, target)

    # read back from the finished zip: what players will unpack
    with zipfile.ZipFile(path) as archive:
        exe = next((item for item in archive.namelist() if item.endswith("Shield_Launcher.exe")), None)
        built = re.search(rb"SHIELD_LAUNCHER_VERSION=([0-9A-Za-z.\-]+)", archive.read(exe)) if exe else None
    if not built or built.group(1).decode() != version:
        p.fail("the launcher in the zip isn't %s" % version)
    return path


def upload_zip(release, path, assets):
    """the new zip first, then the old one goes: old launchers read the release's only zip"""
    name = os.path.basename(path)
    print("uploading %s to the %s release (%s)" % (name, release.release_tag, p.size_text(os.path.getsize(path))), flush=True)
    p.upload_asset(release.repo, release.release_tag, path)
    pattern = release.ship["zip_name"].format(version="*")
    for other in sorted(assets):
        if other != name and fnmatch.fnmatch(other, pattern):
            print("removing the old " + other)
            p.gh("release", "delete-asset", release.release_tag, other, "-R", release.repo, "-y", capture=False)
    folder = release.path(release.ship["zip_folder"])
    os.makedirs(folder, exist_ok=True)
    shutil.copy(path, os.path.join(folder, name))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?", help="the update number, the client has to be at it")
    parser.add_argument("--config", default=os.path.join(TOOLS, "content.json"))
    parser.add_argument("--dry-run", action="store_true", help="only the checks and what would be uploaded")
    parser.add_argument("--allow-stale", action="store_true", help="ship even if a build is older than its source")
    parser.add_argument("--keep-launcher", action="store_true", help="keep the published launcher although its code changed")
    parser.add_argument("--prune", action="store_true", help="delete pieces this and the previous manifest don't use")
    parser.add_argument("--yes", action="store_true", help="don't ask")
    args = parser.parse_args(argv)

    if not args.dry_run:
        p.require_gh()

    release = Release(args.config)
    client = release.client_version()
    version = args.version or client
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        p.fail("the update number looks like 1.1.5, not " + version)

    problems, stale, warnings = [], [], []
    if version != client:
        problems.append("the client is %s, not %s: set it in %s and build the client" % (client, version, release.path(release.config["client_version_file"])))
    if game_running():
        problems.append("Black Ops 4 is running: close it, its files can change while shipping")

    use_gh = not args.dry_run
    latest = p.api_json("repos/%s/releases/latest" % release.repo, use_gh)
    if not latest or latest.get("tag_name") != release.release_tag:
        problems.append("GitHub's latest release isn't %s, old launchers read that one" % release.release_tag)
    zips = p.release_assets(release.repo, release.release_tag, use_gh)
    if zips is None:
        problems.append("the %s release doesn't exist" % release.release_tag)
        zips = {}
    needs_zip = release.zip_name(version) not in zips

    found = check_client(release, version)
    problems += found[0]
    stale += found[1]
    warnings += found[2]
    found = check_support(release)
    problems += found[0]
    stale += found[1]

    assets = p.release_assets(release.repo, release.tag, use_gh)
    plan, plan_problems = launcher_plan(release, version, args.keep_launcher, assets)
    problems += plan_problems

    print("update %s  (launcher %s, client %s)" % (version, p.launcher_version(), client))
    print("launcher: %s" % ("ships this build" if plan == "new" else "stays the published one"))
    print("zip: %s" % (release.zip_name(version) + " goes up" if needs_zip else "already on the %s release" % release.release_tag))
    for line in warnings:
        print("note: " + line)
    for line in stale:
        print(("ignored: " if args.allow_stale else "stale: ") + line)
    for line in problems:
        print("problem: " + line)
    if problems or (stale and not args.allow_stale):
        if stale and not problems:
            print("\nrebuild those, or use --allow-stale to ship anyway")
        if not args.dry_run:
            p.fail("not shipping, nothing was changed")
        print("\nwouldn't ship as it is")
        return "blocked"

    if args.dry_run:
        p.VERSION_OVERRIDE = version
        try:
            p.main(["--config", release.config_path, "--reuse-launcher" if plan == "reuse" else "--no-launcher"])
        finally:
            p.VERSION_OVERRIDE = None
        print("\ndry run: nothing built, written or uploaded")
        return "dry-run"

    # the version only stays changed if the update goes out
    with open(p.VERSION_HEADER, "rb") as file:
        header = file.read()
    shipped = False
    try:
        if p.launcher_version() != version:
            updated = re.sub(rb'(#define\s+LAUNCHER_VERSION\s+")[^"]+(")', lambda m: m.group(1) + version.encode() + m.group(2), header)
            with open(p.VERSION_HEADER, "wb") as file:
                file.write(updated)
            print("launcher version set to " + version)

        zip_path = None
        if plan == "new" or needs_zip:
            print("building the launcher...", flush=True)
            build_launcher()
        if needs_zip:
            zip_path = write_zip(release, version)

        command = ["--config", release.config_path, "--upload"]
        if plan == "reuse":
            command.append("--reuse-launcher")
        if args.yes:
            command.append("--yes")
        if args.prune:
            command.append("--prune")
        note = " and the launcher zip %s" % os.path.basename(zip_path) if zip_path else ""
        result = p.main(command, note)
        if result == "cancelled":
            return "cancelled"
        shipped = True

        if zip_path:
            if result == "unchanged" and not args.yes:
                if not sys.stdin.isatty():
                    p.fail("needs --yes when it can't ask")
                if input("upload %s to the %s release? [y/N] " % (os.path.basename(zip_path), release.release_tag)).strip().lower() not in ("y", "yes"):
                    print("zip not uploaded")
                    return result
            upload_zip(release, zip_path, zips)
        elif result == "unchanged":
            print("nothing to ship")
            return "unchanged"
        print("\nupdate %s shipped" % version)
        return "shipped"
    finally:
        if not shipped:
            with open(p.VERSION_HEADER, "wb") as file:
                file.write(header)


if __name__ == "__main__":
    main()
