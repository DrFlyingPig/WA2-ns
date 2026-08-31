#!/usr/bin/env python3
"""push_via_api.py — 通过 Git Data API 推送仓库(绕过 github.com:443 的 git 协议阻断)

用法:python tools/push_via_api.py [owner/repo] [branch]
读取 git ls-files 的文件列表,逐个上传 blob 后组装 tree/commit/ref。
"""
import base64
import json
import os
import subprocess
import sys
import time
import urllib.request

REPO = sys.argv[1] if len(sys.argv) > 1 else 'DrFlyingPig/WA2-ns'
BRANCH = sys.argv[2] if len(sys.argv) > 2 else 'main'
TOKEN_FILE = os.path.expanduser('~/.gh_token_wa2')
API = f'https://api.github.com/repos/{REPO}'

token = open(TOKEN_FILE).read().strip()


def call(method, url, payload=None, retry=4):
    data = json.dumps(payload).encode() if payload is not None else None
    for attempt in range(retry):
        try:
            req = urllib.request.Request(url, data=data, method=method, headers={
                'Authorization': f'token {token}',
                'Accept': 'application/vnd.github+json',
                'Content-Type': 'application/json',
                'User-Agent': 'wa2-ns-push',
            })
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
                return r.status, json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            body = e.read().decode('utf-8', 'replace')
            if e.code in (409, 422, 404):
                return e.code, json.loads(body) if body else {}
            print(f'  HTTP {e.code}, retry {attempt+1}: {body[:120]}')
            time.sleep(4)
        except Exception as e:
            print(f'  {e}, retry {attempt+1}')
            time.sleep(4)
    sys.exit(f'API 调用彻底失败: {url}')


def main():
    files = subprocess.run(['git', 'ls-files'], capture_output=True, text=True,
                           check=True).stdout.split()
    files = [f for f in files if f]
    print(f'上传 {len(files)} 个文件到 {REPO} ({BRANCH}) ...')

    # 空仓库的 Git Data API 全部 409:用 Contents API 创建首个 commit
    base_tree = None
    readme = base64.b64encode(open('README.md', 'rb').read()).decode()
    status, put = call('PUT', f'{API}/contents/README.md',
                       {'message': 'init: README', 'content': readme})
    if status == 201:
        head_sha = put['commit']['sha']
        print('基线 commit:', head_sha[:12])
    else:
        print('README 已存在或失败:', status, str(put)[:150])
        status, ref = call('GET', f'{API}/git/ref/heads/{BRANCH}')
        head_sha = ref['object']['sha']
    status, head_c = call('GET', f'{API}/git/commits/{head_sha}')
    base_tree = head_c['tree']['sha']
    print('基线 tree:', base_tree[:12])

    tree = []
    for i, path in enumerate(files):
        if path == 'README.md':
            continue   # 已在基线提交中,避免树冲突
        content = open(path, 'rb').read()
        status, blob = call('POST', f'{API}/git/blobs',
                            {'content': base64.b64encode(content).decode(),
                             'encoding': 'base64'})
        if 'sha' not in blob:
            sys.exit(f'blob 失败 {path}: {blob}')
        mode = '100755' if path.endswith('.sh') and os.access(path, os.X_OK) else '100644'
        tree.append({'path': path, 'mode': mode, 'type': 'blob', 'sha': blob['sha']})
        print(f'  [{i+1}/{len(files)}] {path} ({len(content)} bytes)')

    status, t = call('POST', f'{API}/git/trees', {'base_tree': base_tree, 'tree': tree})
    print('tree:', t['sha'][:12])

    msg = subprocess.run(['git', 'log', '-1', '--pretty=%B'],
                         capture_output=True, text=True, check=True).stdout
    status, c0 = call('GET', f'{API}/git/ref/heads/{BRANCH}')
    status, c = call('POST', f'{API}/git/commits',
                     {'message': msg.strip(), 'tree': t['sha'],
                      'parents': [c0['object']['sha']]})
    print('commit:', c['sha'][:12])

    status, r = call('PATCH', f'{API}/git/refs/heads/{BRANCH}', {'sha': c['sha']})
    print('ref 更新:', r.get('object', {}).get('sha', r)[:12] if isinstance(r.get('object', {}).get('sha', r), str) else r.get('object', r))
    print(f'完成: https://github.com/{REPO}/tree/{BRANCH}')


if __name__ == '__main__':
    main()
