#!/usr/bin/env python3

import os

import requests

GH_USERNAME = 'openslide'
PACKAGE_PREFIX = 'vcpkg-cache'

HEADERS = {
    'Accept': 'application/vnd.github+json',
    'Authorization': f'Bearer {os.environ["GITHUB_TOKEN"]}',
    'X-GitHub-Api-Version': '2026-03-10',
}

resp = requests.get(
    f'https://api.github.com/orgs/{GH_USERNAME}/packages?package_type=nuget',
    headers=HEADERS,
)
resp.raise_for_status()
for pkg in sorted(resp.json(), key=lambda pkg: pkg['name']):
    if not pkg['name'].startswith(PACKAGE_PREFIX):
        continue
    resp = requests.get(f'{pkg["url"]}/versions', headers=HEADERS)
    resp.raise_for_status()
    versions = sorted(resp.json(), key=lambda ver: ver['created_at'])
    for ver in versions[:-3]:
        print(
            f'Deleting {pkg["name"]} {ver["name"]} created at {ver["created_at"]}'
        )
        # versions with more than 5k downloads cannot be deleted
        resp = requests.delete(ver['url'], headers=HEADERS)
        if resp.status_code != 204:
            print('...failed:', resp.text)
