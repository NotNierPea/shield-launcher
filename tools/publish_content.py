"""
Builds the launcher's manifest.json and parts from tools/content.json, and publishes them to the content release.

    python tools/publish_content.py                  dry run: hashes everything, says what's new on the release
    python tools/publish_content.py --out DIR        writes manifest.json and the parts into DIR (a local test,
                                                     point the launcher at it with launcher/content-url.txt)
    python tools/publish_content.py --out DIR --only-new
                                                     only the parts the release doesn't have yet, plus manifest.json:
                                                     what to drag onto the release page after a change
    python tools/publish_content.py --upload         uploads the new parts, then manifest.json (needs gh, logged in);
                                                     asks first, --yes skips that. tools/ship.bat runs this
    python tools/publish_content.py --upload --prune also deletes parts neither this nor the previous manifest uses
    python tools/publish_content.py --base-zip FILE  writes the launcher-only download (exe, shortcut, images, sounds)

An entry with "package" (true, or the zip's game path) is zipped into one file instead, which the launcher unzips
in place of the folder: the support mod's Lua, scripts and metadata, so a file taken out of the mod goes away too.

Each file is split into part_size pieces, each uploaded as <sha256>.bin, so a piece that didn't change is never
uploaded or downloaded again. manifest.json goes up last, players never see one whose parts aren't there yet.
"""

import argparse
import atexit
import datetime
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
VERSION_HEADER = os.path.join(ROOT, "source", "launcher", "version.hpp")
CHUNK = 1 << 20
UPLOAD_ATTEMPTS = 4
RETRY_SECONDS = 10
VERSION_OVERRIDE = None  # ship.py's dry run, for a number version.hpp doesn't have yet


def fail(message):
    print("error: " + message, file=sys.stderr)
    sys.exit(1)


def expand(text, variables, base):
    # ${NAME} from content.json's vars (or the environment), relative paths from content.json's folder
    def replace(match):
        name = match.group(1)
        value = os.environ.get(name, variables.get(name))
        if value is None:
            fail("unknown variable ${%s}" % name)
        return expand(value, variables, base)

    text = re.sub(r"\$\{(\w+)\}", replace, text)
    return os.path.normpath(os.path.join(base, text))


WORK = tempfile.mkdtemp(prefix="shield_publish_")
atexit.register(shutil.rmtree, WORK, True)


def folder_files(source, exclude):
    """(path, path relative to source) of a folder's files, sorted, excluded names left out"""
    found = []
    for folder, dirs, names in os.walk(source):
        dirs.sort()
        for name in sorted(names):
            if any(fnmatch.fnmatch(name.lower(), pattern.lower()) for pattern in exclude):
                continue
            path = os.path.join(folder, name)
            found.append((path, os.path.relpath(path, source).replace("\\", "/")))
    return found


def build_package(source, exclude, name):
    """the folder as a zip with fixed dates and order, so the same files always make the same zip (and hash)"""
    path = os.path.join(WORK, name)
    with zipfile.ZipFile(path, "w") as archive:
        for file, relative in folder_files(source, exclude):
            info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 0
            info.external_attr = 0
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(file, "rb") as handle:
                archive.writestr(info, handle.read(), compresslevel=9)
    return path


def collect(entries, variables, base, packages=False):
    """(source, game path, extract folder or None) for every file of the entries, folders taken recursively;
    a "package" entry is one zip (when packages is False, its files are listed one by one, as for the base zip)"""
    files = []
    for entry in entries:
        source = expand(entry["from"], variables, base)
        target = entry["to"].replace("\\", "/").strip("/")
        exclude = entry.get("exclude", [])
        package = entry.get("package")
        if package and packages:
            if not os.path.isdir(source):
                fail("a package has to be a folder: " + source)
            zip_path = package if isinstance(package, str) else "project-bo4/launcher/packages/%s.zip" % os.path.basename(target)
            zip_path = zip_path.replace("\\", "/").strip("/")
            files.append((build_package(source, exclude, os.path.basename(zip_path)), zip_path, target))
        elif os.path.isdir(source):
            for path, relative in folder_files(source, exclude):
                files.append((path, target + "/" + relative, None))
        elif os.path.isfile(source):
            files.append((source, target, None))
        else:
            fail("missing: %s (for %s)" % (source, target))
    return files


def hash_file(path, part_size):
    whole = hashlib.sha256()
    parts = []
    with open(path, "rb") as file:
        while True:
            piece = hashlib.sha256()
            size = 0
            while size < part_size:
                data = file.read(min(CHUNK, part_size - size))
                if not data:
                    break
                whole.update(data)
                piece.update(data)
                size += len(data)
            if size == 0 and parts:
                break
            parts.append({"size": size, "sha256": piece.hexdigest()})
            if size < part_size:
                break
    return {"size": os.path.getsize(path), "sha256": whole.hexdigest(), "parts": parts}


def launcher_source_hash():
    """the launcher's code without its version number: tells a changed launcher from a rebuild of the same one"""
    digest = hashlib.sha256()
    for folder in ("launcher", "utilities"):
        top = os.path.join(ROOT, "source", folder)
        for path, relative in folder_files(top, ["version.hpp"]):
            digest.update(relative.encode())
            with open(path, "rb") as file:
                digest.update(file.read().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def launcher_version():
    if VERSION_OVERRIDE:
        return VERSION_OVERRIDE
    with open(VERSION_HEADER, encoding="utf-8") as file:
        match = re.search(r'#define\s+LAUNCHER_VERSION\s+"([^"]+)"', file.read())
    if not match:
        fail("LAUNCHER_VERSION not found in " + VERSION_HEADER)
    return match.group(1)


def write_parts(path, info, part_size, out_dir, wanted=None):
    """each part of the file as out_dir/<sha256>.bin, those already there (or not wanted) skipped"""
    written = []
    with open(path, "rb") as file:
        for index, part in enumerate(info["parts"]):
            name = part["sha256"] + ".bin"
            dest = os.path.join(out_dir, name)
            if part["size"] == 0 or (wanted is not None and name not in wanted) or (os.path.exists(dest) and os.path.getsize(dest) == part["size"]):
                continue
            file.seek(index * part_size)
            left = part["size"]
            with open(dest + ".tmp", "wb") as out:
                while left:
                    data = file.read(min(CHUNK, left))
                    out.write(data)
                    left -= len(data)
            os.replace(dest + ".tmp", dest)
            written.append(dest)
    return written


GH_MISSING = "the GitHub CLI isn't installed. Run: winget install --id GitHub.cli, open a new terminal, then: gh auth login"


def gh(*args, capture=True):
    try:
        result = subprocess.run(["gh", *args], capture_output=capture, text=True)
    except FileNotFoundError:
        fail(GH_MISSING)
    if result.returncode != 0:
        fail("gh %s failed:\n%s" % (" ".join(args), (result.stderr or "").strip()))
    return result.stdout if capture else ""


def require_gh():
    try:
        result = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True)
    except FileNotFoundError:
        fail(GH_MISSING)
    if result.returncode != 0:
        fail("the GitHub CLI isn't logged in. Run: gh auth login")


def api_json(path, use_gh):
    """a GitHub API answer, None on a 404"""
    if use_gh:
        try:
            result = subprocess.run(["gh", "api", path], capture_output=True, text=True)
        except FileNotFoundError:
            fail(GH_MISSING)
        if result.returncode != 0:
            if "404" in (result.stderr or ""):
                return None
            fail("couldn't read %s:\n%s" % (path, (result.stderr or "").strip()))
        return json.loads(result.stdout)

    # the public api is enough to read a public release (a dry run doesn't need gh)
    request = urllib.request.Request("https://api.github.com/" + path, headers={"User-Agent": "shield-publish-content"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        fail("couldn't read %s: %s" % (path, error))
    except OSError as error:
        fail("couldn't read %s: %s" % (path, error))


def release_assets(repo, tag, use_gh):
    """{name: size} of the fully uploaded assets (a broken upload doesn't count), None when the release doesn't exist yet"""
    release = api_json("repos/%s/releases/tags/%s" % (repo, tag), use_gh)
    if release is None:
        return None
    assets = {}
    page = 1
    while True:
        batch = api_json("repos/%s/releases/%d/assets?per_page=100&page=%d" % (repo, release["id"], page), use_gh) or []
        assets.update({asset["name"]: asset["size"] for asset in batch if asset.get("state", "uploaded") == "uploaded"})
        if len(batch) < 100:
            return assets
        page += 1


def upload_asset(repo, tag, path):
    """retried, a long upload can drop; --clobber also replaces a broken earlier try"""
    error = ""
    for attempt in range(1, UPLOAD_ATTEMPTS + 1):
        try:
            result = subprocess.run(["gh", "release", "upload", tag, path, "-R", repo, "--clobber"], capture_output=True, text=True)
        except FileNotFoundError:
            fail(GH_MISSING)
        if result.returncode == 0:
            return
        error = (result.stderr or "").strip()
        if attempt < UPLOAD_ATTEMPTS:
            print("  failed (try %d of %d), retrying: %s" % (attempt, UPLOAD_ATTEMPTS, error.splitlines()[-1] if error else "no message"))
            time.sleep(RETRY_SECONDS * attempt)
    fail("couldn't upload %s:\n%s\nmanifest.json wasn't changed unless this was it. Run the script again, it continues." % (os.path.basename(path), error))


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for data in iter(lambda: file.read(CHUNK), b""):
            digest.update(data)
    return digest.hexdigest()


def live_manifest(repo, tag):
    try:
        url = "https://github.com/%s/releases/download/%s/manifest.json" % (repo, tag)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "shield-publish-content"}), timeout=20) as response:
            return json.load(response)
    except Exception:
        return None


def manifest_parts(manifest):
    names = set()
    if not manifest:
        return names
    entries = list(manifest.get("files", []))
    if manifest.get("launcher", {}).get("file"):
        entries.append(manifest["launcher"]["file"])
    for entry in entries:
        # an empty file has an empty part, nothing to upload for it
        names.update(part["sha256"] + ".bin" for part in entry.get("parts", []) if part["size"] > 0)
    return names


def size_text(size):
    return "%.2f GB" % (size / 2 ** 30) if size >= 2 ** 30 else "%.1f MB" % (size / 2 ** 20)


def main(argv=None, note=""):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default=os.path.join(TOOLS, "content.json"))
    parser.add_argument("--version", help="the content version, the date and time by default")
    parser.add_argument("--out", help="write manifest.json and the parts into this folder")
    parser.add_argument("--only-new", action="store_true", help="with --out, write only the parts the release doesn't have yet (into an empty folder)")
    parser.add_argument("--upload", action="store_true", help="upload the new parts and manifest.json with gh")
    parser.add_argument("--yes", action="store_true", help="with --upload, don't ask before publishing")
    parser.add_argument("--prune", action="store_true", help="with --upload, delete parts the new and previous manifests don't use")
    parser.add_argument("--no-launcher", action="store_true", help="leave the launcher out of the manifest (no self-update)")
    parser.add_argument("--reuse-launcher", action="store_true", help="keep the launcher entry of the live manifest")
    parser.add_argument("--base-zip", help="also write the launcher-only download zip here")
    args = parser.parse_args(argv)
    if args.upload:
        require_gh()

    with open(args.config, encoding="utf-8") as file:
        config = json.load(file)
    base = os.path.dirname(os.path.abspath(args.config))
    variables = config.get("vars", {})
    part_size = int(config.get("part_size", 256 * 2 ** 20))
    repo, tag = config["repo"], config["tag"]

    # every file, hashed
    manifest = {
        "schema": 1,
        "version": args.version or datetime.datetime.now().strftime("%Y.%m.%d-%H%M"),
        "part_size": part_size,
        "components": [],
        "files": [],
        "remove": config.get("remove", []),
    }
    sources = {}
    seen = {}
    for component in config["components"]:
        manifest["components"].append({key: component[key] for key in ("id", "name", "description", "required") if key in component})
        for source, target, extract in collect(component["files"], variables, base, packages=True):
            if target.lower() in seen:
                fail("%s comes from both %s and %s" % (target, seen[target.lower()], source))
            seen[target.lower()] = source
            print("hashing " + target)
            info = hash_file(source, part_size)
            entry = {"path": target, "component": component["id"], **info}
            if extract:
                # the launcher unzips it into this folder, replacing what was there, and checks the folder by this list
                with zipfile.ZipFile(source) as archive:
                    entry["extract"] = extract
                    entry["entries"] = [{"path": item.filename, "size": item.file_size, "crc32": item.CRC}
                                        for item in archive.infolist() if not item.is_dir()]
            manifest["files"].append(entry)
            sources[target] = (source, info)

    if args.reuse_launcher:
        published = live_manifest(repo, tag)
        if not published or not published.get("launcher"):
            fail("there's no published launcher to keep")
        manifest["launcher"] = published["launcher"]
    elif not args.no_launcher and config.get("launcher"):
        exe = expand(config["launcher"], variables, base)
        if not os.path.isfile(exe):
            fail("launcher not built: " + exe + " (build Release, or pass --no-launcher)")
        # a manifest newer than its exe would offer the same update forever
        version = launcher_version()
        # the launcher carries the client's version number
        client_file = expand(config["client_version_file"], variables, base) if config.get("client_version_file") else None
        if client_file and os.path.isfile(client_file):
            with open(client_file, encoding="utf-8") as file:
                client = re.search(r'version\s*=\s*"([^"]+)"', file.read())
            if client and client.group(1) != version:
                fail("the launcher is %s but the client is %s (%s): set LAUNCHER_VERSION in version.hpp and build Release"
                     % (version, client.group(1), client_file))
        with open(exe, "rb") as file:
            built = re.search(rb"SHIELD_LAUNCHER_VERSION=([0-9A-Za-z.\-]+)", file.read())
        if not built or built.group(1).decode() != version:
            fail("%s is version %s, version.hpp says %s: build Release again" % (exe, built.group(1).decode() if built else "unknown", version))
        info = hash_file(exe, part_size)
        manifest["launcher"] = {"version": version, "source": launcher_source_hash(), "file": info}
        sources["<launcher>"] = (exe, info)

    # the client dll has to be the build of this version: its watermark keeps the version as a string of its own
    if config.get("client_dll") and config["client_dll"] in sources:
        version = launcher_version()
        dll = sources[config["client_dll"]][0]
        with open(dll, "rb") as file:
            data = file.read()
        if b"\x00" + version.encode() + b"\x00" not in data:
            built = sorted({m.group(1).decode() for m in re.finditer(rb"\x00(\d+\.\d+\.\d+)\x00", data)})
            fail("%s isn't the %s client (it has %s): build the client first" % (dll, version, ", ".join(built) or "no version"))

    total = sum(entry["size"] for entry in manifest["files"])
    print("\n%d files, %s, content version %s" % (len(manifest["files"]), size_text(total), manifest["version"]))
    for component in manifest["components"]:
        files = [entry for entry in manifest["files"] if entry["component"] == component["id"]]
        print("  %-16s %4d files  %s" % (component["id"], len(files), size_text(sum(entry["size"] for entry in files))))
    if "launcher" in manifest:
        print("  launcher         %s" % manifest["launcher"]["version"])

    needed = manifest_parts(manifest)
    part_sizes = {}
    for entry in list(manifest["files"]) + ([manifest["launcher"]["file"]] if "launcher" in manifest else []):
        for part in entry["parts"]:
            part_sizes[part["sha256"] + ".bin"] = part["size"]

    if args.base_zip:
        with zipfile.ZipFile(args.base_zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for source, target, _ in collect(config["base_zip"], variables, base):
                archive.write(source, target)
        print("\nwrote " + args.base_zip + " (" + size_text(os.path.getsize(args.base_zip)) + ")")

    existing = release_assets(repo, tag, args.upload)
    # a part counts as there only when it's whole
    new_parts = {name for name in needed if (existing or {}).get(name) != part_sizes[name]}
    print("\nrelease %s/%s: %s, %d of %d parts new (%s to upload)" % (
        repo, tag, "not created yet" if existing is None else "%d assets" % len(existing),
        len(new_parts), len(needed), size_text(sum(part_sizes[name] for name in new_parts))))
    if existing:
        unused = sorted(name for name in existing if name.endswith(".bin") and name not in needed)
        if unused:
            print("%d parts on the release aren't used by this manifest anymore, they can be deleted once it's up" % len(unused))

    if args.out:
        # --only-new: just what the release doesn't have, ready to drag onto it with the new manifest.json
        if args.only_new and os.path.isdir(args.out) and os.listdir(args.out):
            fail(args.out + " isn't empty: --only-new needs an empty folder, so everything in it is to upload")
        os.makedirs(args.out, exist_ok=True)
        wanted = new_parts if args.only_new else None
        for source, info in sources.values():
            write_parts(source, info, part_size, args.out, wanted)
        with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as file:
            json.dump(manifest, file, indent=1)
        print("\nwrote manifest.json and %d parts to %s" % (len(wanted) if args.only_new else len(needed), args.out))
        if args.only_new:
            print("on the %s release: upload the .bin files first, then delete the old manifest.json and upload the new one" % tag)

    if not args.upload:
        if not args.out:
            print("dry run, nothing written. --upload publishes, --out DIR writes a local copy.")
        return "dry-run"

    # the same files as what's live: nothing for players to download, so nothing to publish
    previous = live_manifest(repo, tag)
    same = previous is not None and not new_parts and all(previous.get(key) == manifest.get(key) for key in ("files", "launcher", "components", "remove"))
    if same:
        print("\nnothing changed since content %s, nothing to publish" % previous.get("version"))
    else:
        if not args.yes:
            if not sys.stdin.isatty():
                fail("--upload needs --yes when it can't ask")
            answer = input("\npublish content %s to players, uploading %s%s? [y/N] " % (
                manifest["version"], size_text(sum(part_sizes[name] for name in new_parts)), note))
            if answer.strip().lower() not in ("y", "yes"):
                print("cancelled, nothing uploaded")
                return "cancelled"

        if existing is None:
            gh("release", "create", tag, "-R", repo, "--title", "Game files", "--latest=false",
               "--notes", "Files the Shield launcher downloads (manifest.json and its parts). Not a download for players.")

        with tempfile.TemporaryDirectory(prefix="shield_content_") as temp:
            # the parts first, each written out only for its upload and checked: a file rebuilt since hashing would be a bad part
            done = 0
            for source, info in sources.values():
                mine = {part["sha256"] + ".bin" for part in info["parts"]} & new_parts
                if not mine:
                    continue
                for path in write_parts(source, info, part_size, temp, mine):
                    name = os.path.basename(path)
                    if file_sha256(path) + ".bin" != name:
                        fail("%s changed while publishing, nothing went live. Run the script again." % source)
                    done += 1
                    print("uploading %d of %d: %s (%s)" % (done, len(new_parts), name, size_text(os.path.getsize(path))), flush=True)
                    upload_asset(repo, tag, path)
                    os.remove(path)
                new_parts -= mine

            # every part has to be whole on the release before players are pointed at it
            uploaded = release_assets(repo, tag, True) or {}
            missing = sorted(name for name in needed if uploaded.get(name) != part_sizes[name])
            if missing:
                fail("%d parts aren't whole on the release (%s...), manifest.json wasn't changed. Run the script again." % (len(missing), missing[0][:16]))

            # a dated copy first, then manifest.json: the one upload that goes live
            manifest_path = os.path.join(temp, "manifest.json")
            with open(manifest_path, "w", encoding="utf-8") as file:
                json.dump(manifest, file, indent=1)
            dated = os.path.join(temp, "manifest-%s.json" % manifest["version"])
            shutil.copy(manifest_path, dated)
            upload_asset(repo, tag, dated)
            upload_asset(repo, tag, manifest_path)

        live = None
        for _ in range(6):
            live = live_manifest(repo, tag)
            if live and live.get("version") == manifest["version"]:
                break
            time.sleep(RETRY_SECONDS / 2)
        if live and live.get("version") == manifest["version"]:
            print("\npublished content %s, it's live" % manifest["version"])
        else:
            print("\nuploaded content %s, but the download link doesn't show it yet: check it in a minute" % manifest["version"])

    if args.prune:
        # without the previous manifest, a player mid-download could still need any part
        if previous is None:
            print("not pruning: the previous manifest couldn't be read")
            return "unchanged" if same else "published"
        keep = needed | manifest_parts(previous)
        current = release_assets(repo, tag, True) or {}
        stale = sorted(name for name in current if name.endswith(".bin") and name not in keep)
        for name in stale:
            print("deleting " + name)
            gh("release", "delete-asset", tag, name, "-R", repo, "-y", capture=False)
        print("pruned %d parts" % len(stale))
    return "unchanged" if same else "published"


if __name__ == "__main__":
    main()
