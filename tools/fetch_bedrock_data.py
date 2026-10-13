#!/usr/bin/env python3 -I
"""Generate Painite's Bedrock join data from Mojang's own Bedrock Dedicated Server.

Downloads the current Linux BDS, runs it offline on a flat world, joins it with tools/bedrock_capture.py and writes
data/bedrock-<version>.bin. Nothing from Mojang is stored in the repository; the file is git-ignored.

usage: tools/fetch_bedrock_data.py [--out data] [--port 19199] [--preview]
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, time, urllib.request, zipfile

LINKS = "https://net-secondary.web.minecraft-services.net/api/v1.0/download/links"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data")
    ap.add_argument("--port", type=int, default=19199)
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()
    req = urllib.request.Request(LINKS, headers={"User-Agent": "painite-data-tool"})
    links = json.load(urllib.request.urlopen(req, timeout=60))["result"]["links"]
    kind = "serverBedrockPreviewLinux" if a.preview else "serverBedrockLinux"
    url = next(l["downloadUrl"] for l in links if l["downloadType"] == kind)
    version = re.search(r"bedrock-server-([\d.]+)\.zip", url).group(1)
    work = tempfile.mkdtemp(prefix="painite-bds-")
    try:
        print("downloading", url)
        zpath = os.path.join(work, "bds.zip")
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=600) as r, open(zpath, "wb") as f:
            shutil.copyfileobj(r, f)
        run = os.path.join(work, "run")
        with zipfile.ZipFile(zpath) as z:
            z.extractall(run)
        exe = os.path.join(run, "bedrock_server")
        os.chmod(exe, 0o755)
        props = os.path.join(run, "server.properties")
        text = open(props).read()
        for k, v in {"online-mode": "false", "allow-list": "false", "server-port": str(a.port), "server-portv6": str(a.port + 1),
                     "transport": "raknet", "level-name": "capture", "view-distance": "6", "level-type": "FLAT"}.items():
            if re.search(rf"^{re.escape(k)}=", text, re.M):
                text = re.sub(rf"^{re.escape(k)}=.*$", f"{k}={v}", text, flags=re.M)
            else:
                text += f"\n{k}={v}\n"
        open(props, "w").write(text)
        log = open(os.path.join(work, "bds.log"), "w")
        env = dict(os.environ, LD_LIBRARY_PATH=run)
        srv = subprocess.Popen([exe], cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if "Server started" in open(os.path.join(work, "bds.log")).read():
                    break
                time.sleep(1)
            else:
                sys.exit("Bedrock Dedicated Server did not start")
            time.sleep(1)
            here = os.path.dirname(os.path.abspath(__file__))
            os.makedirs(a.out, exist_ok=True)
            # the version in the file name is what Painite shows; the game version is the BDS release
            target = os.path.join(a.out, f"bedrock-{version}.bin")
            subprocess.run([sys.executable, os.path.join(here, "bedrock_capture.py"), "127.0.0.1", str(a.port), target, "8"], check=True)
        finally:
            srv.terminate()
            try:
                srv.wait(timeout=30)
            except subprocess.TimeoutExpired:
                srv.kill()
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
