#!/usr/bin/env node
/**
 * Star History 数据初始化 + 每日快照
 *
 * 用法：
 *   node star-history.mjs init <owner>/<repo> [outputDir]   # 一次性：拉全量历史
 *   node star-history.mjs snapshot <owner>/<repo> [outputDir] # 每日：只读 stargazers_count 追加当天
 *
 * 数据源：
 *   init     用 GITHUB_TOKEN 拉 stargazer 列表（Accept: star+json，含 starred_at）
 *   snapshot 读公开的 stargazers_count，追加 {date, count} 快照
 *
 * 产物：
 *   <outputDir>/history.json  累计数据
 *   <outputDir>/star-history.svg  生成的图表
 *
 * 无第三方依赖，仅用 Node 内置 fetch。
 */

import fs from 'fs';
import path from 'path';

const TOKEN = process.env.GITHUB_TOKEN || process.env.STAR_HISTORY_TOKEN;
const BASE = 'https://api.github.com';

const UA = { 'User-Agent': 'star-history-action', 'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28' };
const authHeaders = TOKEN ? { ...UA, Authorization: `Bearer ${TOKEN}` } : UA;

async function gh(pathname, opts = {}) {
  const headers = opts.auth === false ? { ...UA, ...(opts.headers || {}) } : { ...authHeaders, ...(opts.headers || {}) };
  const res = await fetch(`${BASE}${pathname}`, { headers });
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`GET ${pathname} → ${res.status}: ${body.slice(0, 200)}`);
  }
  return res.json();
}

// 拉取全部 stargazer（分页），返回 [{starred_at, login}]
async function fetchAllStargazers(owner, repo) {
  const out = [];
  let page = 1;
  for (;;) {
    const items = await gh(`/repos/${owner}/${repo}/stargazers?per_page=100&page=${page}`, {
      headers: { Accept: 'application/vnd.github.star+json' }
    });
    if (!items.length) break;
    for (const it of items) {
      out.push({ at: it.starred_at, login: it.login });
    }
    if (items.length < 100) break;
    page++;
    if (page > 20) break; // 安全上限 2000 star
  }
  return out;
}

// 把 stargazer 列表转成 {date, count} 序列（按天）
function stargazersToSeries(users) {
  const days = {};
  for (const u of users) {
    const d = (u.at || '').slice(0, 10); // YYYY-MM-DD
    if (!d) continue;
    days[d] = (days[d] || 0) + 1;
  }
  const dates = Object.keys(days).sort();
  const series = [];
  let count = 0;
  for (const d of dates) {
    count += days[d];
    series.push({ date: d, count });
  }
  return series;
}

function loadHistory(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return [];
  }
}

async function cmdInit(owner, repo, dir) {
  console.log(`[init] 拉取 ${owner}/${repo} 全部 stargazer 历史...`);
  const users = await fetchAllStargazers(owner, repo);
  const series = stargazersToSeries(users);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'history.json'), JSON.stringify(series, null, 2));
  console.log(`[init] 完成：${series.length} 天数据，末值 ${series.length ? series[series.length - 1].count : 0}`);
}

async function cmdSnapshot(owner, repo, dir) {
  fs.mkdirSync(dir, { recursive: true });
  const historyPath = path.join(dir, 'history.json');
  const history = loadHistory(historyPath);

  // 读当前 star 数（公开数据，无需 token 也可；有 token 更稳）
  const info = await gh(`/repos/${owner}/${repo}`);
  const today = new Date().toISOString().slice(0, 10);
  const count = info.stargazers_count || 0;

  // 追加/覆盖今天
  const last = history.length ? history[history.length - 1] : null;
  if (last && last.date === today) {
    last.count = count;
    console.log(`[snapshot] ${today} 已存在，更新为 ${count}`);
  } else {
    history.push({ date: today, count });
    console.log(`[snapshot] 追加 ${today}: ${count}`);
  }
  fs.writeFileSync(historyPath, JSON.stringify(history, null, 2));
}

async function main() {
  const [cmd, arg, outDir] = process.argv.slice(2);
  if (!cmd || !arg) {
    console.error('用法: node star-history.mjs <init|snapshot> <owner/repo> [outputDir]');
    process.exit(1);
  }
  const dir = outDir || 'star-history';
  const [owner, repo] = arg.split('/');
  if (!owner || !repo) {
    console.error('仓库格式: owner/repo');
    process.exit(1);
  }
  if (cmd === 'init') await cmdInit(owner, repo, dir);
  else if (cmd === 'snapshot') await cmdSnapshot(owner, repo, dir);
  else { console.error(`未知命令: ${cmd}`); process.exit(1); }
}

main().catch((e) => { console.error(e.message); process.exit(1); });
