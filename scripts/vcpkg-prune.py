#!/usr/bin/env python3

import itertools
import os

import requests

GH_USERNAME = 'openslide'
PACKAGE_PREFIX = 'vcpkg-cache'

HEADERS = {
    'Accept': 'application/vnd.github+json',
    'Authorization': f'Bearer {os.environ["GITHUB_TOKEN"]}',
    'X-GitHub-Api-Version': '2026-03-10',
}

for page in itertools.count(1):
    resp = requests.get(
        f'https://api.github.com/orgs/{GH_USERNAME}/packages?package_type=nuget&page={page}',
        headers=HEADERS,
    )
    resp.raise_for_status()
    packages = resp.json()
    if not packages:
        break
    for pkg in packages:
        if not pkg['name'].startswith(PACKAGE_PREFIX):
            continue
        while True:
            resp = requests.get(f'{pkg["url"]}/versions', headers=HEADERS)
            resp.raise_for_status()
            prune = sorted(resp.json(), key=lambda ver: ver['created_at'])[:-3]
            if not prune:
                break
            for ver in prune:
                print(
                    f'Deleting {pkg["name"]} {ver["name"]} created at {ver["created_at"]}'
                )
                # versions with more than 5k downloads cannot be deleted
                resp = requests.delete(ver['url'], headers=HEADERS)
                if resp.status_code != 204:
                    print('...failed:', resp.text)
