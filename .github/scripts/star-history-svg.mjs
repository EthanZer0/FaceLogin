#!/usr/bin/env node
/**
 * Star History 图表生成器（复刻 star-history.com 的 xkcd 手绘风格）
 *
 * 用法：
 *   node star-history-svg.mjs <history.json> <output.svg>
 *
 * 读 history.json（[{date, count}]），生成手绘风 SVG：
 *   - xkcd 手绘字体（内嵌 woff，base64）
 *   - feTurbulence + feDisplacementMap 手绘描边 filter
 *   - 相对时间轴（day/week/month/year，"day one"、"5 days"...）
 *   - 主色 #dd4528（红），支持浅/深色主题（prefers-color-scheme）
 */

import fs from 'fs';

// ============ xkcd 手绘字体（woff base64） ============
// 来自 xkcd font (https://github.com/ipython/xkcd-font) 的 "xkcd" woff
// 这是 star-history 使用的同一字体。为避免外部依赖，内嵌 base64。
// 说明：完整字体较大，这里使用兼容的占位字体名，实际渲染时回退到系统手写体。
const XKCD_FONT_B64 = ''; // 空 → 使用下方 @font-face 的本地回退

// ============ 颜色（复刻 star-history） ============
const COLORS = {
  light: {
    line: '#dd4528',      // 主红
    bg: '#ffffff',
    stroke: '#000000',
    grid: '#00000022',
    text: '#000000',
  },
  dark: {
    line: '#ff6b6b',      // 主红（暗色）
    bg: '#0d1117',        // GitHub 暗色背景
    stroke: '#ffffff',
    grid: '#ffffff22',
    text: '#ffffff',
  },
};

// ============ SVG 布局参数 ============
const W = 800;      // 画布宽
const H = 450;      // 画布高
const M = { top: 50, right: 40, bottom: 60, left: 80 }; // margin

// ============ 工具 ============
function scaleLinear(domain, range) {
  const [d0, d1] = domain;
  const [r0, r1] = range;
  const k = (r1 - r0) / (d1 - d0 || 1);
  return (v) => r0 + (v - d0) * k;
}

function fmtNumber(n) {
  if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
  if (n >= 10000) return (n / 1000).toFixed(1) + 'k';
  if (n >= 1000) return Math.round(n / 1000 * 10) / 10 + 'k';
  return String(n);
}

// 相对时间格式（复刻 star-history 的 getFormatTimeline）
function fmtRelative(daysAgo) {
  if (daysAgo <= 0) return 'day one';
  if (daysAgo < 7) return daysAgo === 1 ? 'a day' : `${daysAgo} days`;
  const weeks = Math.floor(daysAgo / 7);
  if (weeks < 5) return weeks === 1 ? 'a week' : `${weeks} weeks`;
  const months = Math.round(daysAgo / 30);
  if (months < 12) return months === 1 ? 'a month' : `${months} months`;
  const years = Math.round(daysAgo / 365);
  return years === 1 ? 'a year' : `${years} years`;
}

// 平滑折线（Catmull-Rom → Bezier），模拟 d3 curveMonotoneX
function smoothPath(points) {
  if (points.length < 2) return '';
  let d = `M${points[0][0]},${points[0][1]}`;
  for (let i = 1; i < points.length; i++) {
    const p0 = points[i - 1];
    const p1 = points[i];
    // 简单的二次贝塞尔控制点（取中间）
    const cx = (p0[0] + p1[0]) / 2;
    d += ` C ${cx},${p0[1]} ${cx},${p1[1]} ${p1[0]},${p1[1]}`;
  }
  return d;
}

// ============ 生成 SVG ============
function renderSVG(history, theme) {
  const C = COLORS[theme];
  const series = history.map(h => ({ x: new Date(h.date).getTime(), y: h.count }));
  if (!series.length) return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}"><text x="20" y="30">No data</text></svg>`;

  const minT = series[0].x;
  const maxT = series[series.length - 1].x;
  const maxY = Math.max(...series.map(s => s.y), 1);
  const yMax = Math.ceil(maxY * 1.1); // 留 10% 顶距

  const plotW = W - M.left - M.right;
  const plotH = H - M.top - M.bottom;

  const xScale = scaleLinear([minT, maxT], [M.left, M.left + plotW]);
  const yScale = scaleLinear([0, yMax], [M.top + plotH, M.top]);

  const pts = series.map(s => [xScale(s.x), yScale(s.y)]);
  const path = smoothPath(pts);

  // 坐标轴刻度（5 个 x 刻度：相对时间；5 个 y 刻度：star 数）
  const xTicks = [];
  for (let i = 0; i < 5; i++) {
    const t = minT + (maxT - minT) * (i / 4);
    const daysAgo = Math.round((maxT - t) / 86400000);
    xTicks.push({ x: xScale(t), label: fmtRelative(daysAgo) });
  }
  const yTicks = [];
  for (let i = 0; i <= 5; i++) {
    const v = yMax * (i / 5);
    yTicks.push({ y: yScale(v), label: fmtNumber(v) });
  }

  // 图例（top-left）
  const legendX = M.left + 10;
  const legendY = M.top - 22;

  return `  <rect x="0" y="0" width="${W}" height="${H}" fill="${C.bg}"/>

  <!-- 网格线 -->
  ${yTicks.map(t => `<line x1="${M.left}" y1="${t.y}" x2="${M.left + plotW}" y2="${t.y}" stroke="${C.grid}" stroke-width="1"/>`).join('\n')}

  <!-- X 轴刻度 -->
  ${xTicks.map(t => `<line x1="${t.x}" y1="${M.top + plotH}" x2="${t.x}" y2="${M.top + plotH + 6}" stroke="${C.stroke}" stroke-width="1.5"/>`).join('\n')}
  <!-- Y 轴刻度 -->
  ${yTicks.map(t => `<line x1="${M.left - 6}" y1="${t.y}" x2="${M.left}" y2="${t.y}" stroke="${C.stroke}" stroke-width="1.5"/>`).join('\n')}

  <!-- X 轴标签 -->
  ${xTicks.map(t => `<text x="${t.x}" y="${M.top + plotH + 28}" text-anchor="middle" font-size="16" fill="${C.text}">${t.label}</text>`).join('\n')}
  <!-- Y 轴标签 -->
  ${yTicks.map(t => `<text x="${M.left - 14}" y="${t.y + 5}" text-anchor="end" font-size="16" fill="${C.text}">${t.label}</text>`).join('\n')}

  <!-- 坐标轴框线 -->
  <line x1="${M.left}" y1="${M.top + plotH}" x2="${M.left + plotW}" y2="${M.top + plotH}" stroke="${C.stroke}" stroke-width="2" filter="url(#xkcdify)"/>
  <line x1="${M.left}" y1="${M.top}" x2="${M.left}" y2="${M.top + plotH}" stroke="${C.stroke}" stroke-width="2" filter="url(#xkcdify)"/>

  <!-- 数据折线（手绘风） -->
  <path d="${path}" fill="none" stroke="${C.line}" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round" filter="url(#xkcdify)"/>

  <!-- 数据点 -->
  ${pts.map(p => `<circle cx="${p[0]}" cy="${p[1]}" r="3" fill="${C.line}"/>`).join('\n')}

  <!-- 图例 -->
  <line x1="${legendX}" y1="${legendY}" x2="${legendX + 24}" y2="${legendY}" stroke="${C.line}" stroke-width="3.5" filter="url(#xkcdify)"/>
  <circle cx="${legendX + 12}" cy="${legendY}" r="3" fill="${C.line}"/>
  <text x="${legendX + 30}" y="${legendY + 5}" font-size="16" fill="${C.text}">FaceLogin</text>

  <!-- 水印 -->
  <text x="${W - 10}" y="${H - 12}" text-anchor="end" font-size="12" fill="${C.text}" opacity="0.4">star-history-action</text>
`;
}

// ============ 主函数 ============
function main() {
  const [histPath, outPath] = process.argv.slice(2);
  if (!histPath || !outPath) {
    console.error('用法: node star-history-svg.mjs <history.json> <output.svg>');
    process.exit(1);
  }
  const history = JSON.parse(fs.readFileSync(histPath, 'utf8'));
  // 生成包含 light + dark 两套主题的 SVG（用 CSS 媒体查询切换）
  const light = renderSVG(history, 'light').replace(/url\(#xkcdify\)/g, 'url(#xkcdify-light)');
  const dark = renderSVG(history, 'dark').replace(/url\(#xkcdify\)/g, 'url(#xkcdify-dark)');
  const combined = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" font-family="xkcd, 'Comic Neue', 'Segoe Print', cursive">
  <defs>
    <style>
      @font-face { font-family: "xkcd"; src: local("Segoe Print"), local("Comic Neue"), local("Comic Sans MS"); }
    </style>
    <filter id="xkcdify-light" filterUnits="userSpaceOnUse" x="-5" y="-5" width="100%" height="100%">
      <feTurbulence type="fractalNoise" baseFrequency="0.05" result="noise"/>
      <feDisplacementMap scale="5" xChannelSelector="R" yChannelSelector="G" in="SourceGraphic" in2="noise"/>
    </filter>
    <filter id="xkcdify-dark" filterUnits="userSpaceOnUse" x="-5" y="-5" width="100%" height="100%">
      <feTurbulence type="fractalNoise" baseFrequency="0.05" result="noise"/>
      <feDisplacementMap scale="5" xChannelSelector="R" yChannelSelector="G" in="SourceGraphic" in2="noise"/>
    </filter>
    <style>
      .theme-light { display: inline; }
      .theme-dark { display: none; }
      @media (prefers-color-scheme: dark) {
        .theme-light { display: none; }
        .theme-dark { display: inline; }
      }
    </style>
  </defs>
  <g class="theme-light">${light}</g>
  <g class="theme-dark">${dark}</g>
</svg>`;
  fs.writeFileSync(outPath, combined);
  console.log(`已生成 ${outPath}`);
}

main();
