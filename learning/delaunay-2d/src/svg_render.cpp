#include "svg_render.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

struct ViewBox {
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    double scaleX = 1, scaleY = 1;
    double width = 820, height = 620;
};

ViewBox computeView(const Step& step) {
    double minx = step.points[0].x, maxx = step.points[0].x;
    double miny = step.points[0].y, maxy = step.points[0].y;
    for (int i = 1; i < step.realPointCount; ++i) {
        minx = std::min(minx, step.points[i].x);
        maxx = std::max(maxx, step.points[i].x);
        miny = std::min(miny, step.points[i].y);
        maxy = std::max(maxy, step.points[i].y);
    }
    const double pad = std::max(maxx - minx, maxy - miny) * 0.08 + 1e-6;
    ViewBox v;
    v.minx = minx - pad;
    v.maxx = maxx + pad;
    v.miny = miny - pad;
    v.maxy = maxy + pad;
    v.width = 820;
    v.height = v.width * (v.maxy - v.miny) / (v.maxx - v.minx);
    v.scaleX = v.width / (v.maxx - v.minx);
    v.scaleY = v.height / (v.maxy - v.miny);
    return v;
}

double sx(const ViewBox& v, double x) { return (x - v.minx) * v.scaleX; }
double sy(const ViewBox& v, double y) { return v.height - (y - v.miny) * v.scaleY; }

std::string num(double x) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << x;
    return o.str();
}

// XML 文本转义（用于说明文字）
std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

bool sameTriangle(const Triangle& a, const Triangle& b) {
    int av[3] = {a.v[0], a.v[1], a.v[2]};
    int bv[3] = {b.v[0], b.v[1], b.v[2]};
    std::sort(av, av + 3);
    std::sort(bv, bv + 3);
    return av[0] == bv[0] && av[1] == bv[1] && av[2] == bv[2];
}

}  // namespace

std::string stepToSVG(const Step& step, int frameIndex, int totalFrames) {
    const ViewBox v = computeView(step);
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << v.width
      << "\" height=\"" << v.height << "\" viewBox=\"0 0 " << v.width << " " << v.height
      << "\">\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

    // 1) 被破坏三角形的外接圆（红色虚线）
    for (const Circle& c : step.removedCircles) {
        o << "<circle cx=\"" << num(sx(v, c.cx)) << "\" cy=\"" << num(sy(v, c.cy))
          << "\" r=\"" << num(c.r * v.scaleX)
          << "\" fill=\"none\" stroke=\"#ef4444\" stroke-width=\"1.2\""
          << " stroke-dasharray=\"4,3\" opacity=\"0.55\"/>\n";
    }

    // 2) 当前三角形
    for (const Triangle& t : step.triangles) {
        bool isNew = false;
        for (const Triangle& nt : step.newTriangles) {
            if (sameTriangle(t, nt)) { isNew = true; break; }
        }
        const bool usesSuper =
            t.v[0] >= step.realPointCount || t.v[1] >= step.realPointCount ||
            t.v[2] >= step.realPointCount;
        const std::string fill = isNew ? "#b9f6ca" : (usesSuper ? "#e2e8f0" : "#cfe8ff");
        const std::string stroke = isNew ? "#1b7f3b" : (usesSuper ? "#94a3b8" : "#2b6cb0");
        o << "<polygon points=\""
          << num(sx(v, step.points[t.v[0]].x)) << "," << num(sy(v, step.points[t.v[0]].y)) << " "
          << num(sx(v, step.points[t.v[1]].x)) << "," << num(sy(v, step.points[t.v[1]].y)) << " "
          << num(sx(v, step.points[t.v[2]].x)) << "," << num(sy(v, step.points[t.v[2]].y))
          << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"1.4\""
          << (usesSuper ? " stroke-dasharray=\"5,3\"" : "") << " stroke-linejoin=\"round\"/>\n";
    }

    // 3) 空腔边界边（橙色粗线）
    for (const Edge& e : step.cavityEdges) {
        o << "<line x1=\"" << num(sx(v, step.points[e.a].x))
          << "\" y1=\"" << num(sy(v, step.points[e.a].y))
          << "\" x2=\"" << num(sx(v, step.points[e.b].x))
          << "\" y2=\"" << num(sy(v, step.points[e.b].y))
          << "\" stroke=\"#f97316\" stroke-width=\"3\" stroke-linecap=\"round\"/>\n";
    }

    // 4) 超三角形轮廓（灰色虚线，只有初始化/插入阶段可见）
    if (step.points.size() > static_cast<size_t>(step.realPointCount)) {
        const int s0 = step.realPointCount, s1 = s0 + 1, s2 = s0 + 2;
        const int supers[3][2] = {{s0, s1}, {s1, s2}, {s2, s0}};
        for (const auto& e : supers) {
            o << "<line x1=\"" << num(sx(v, step.points[e[0]].x))
              << "\" y1=\"" << num(sy(v, step.points[e[0]].y))
              << "\" x2=\"" << num(sx(v, step.points[e[1]].x))
              << "\" y2=\"" << num(sy(v, step.points[e[1]].y))
              << "\" stroke=\"#94a3b8\" stroke-width=\"1\" stroke-dasharray=\"6,4\" opacity=\"0.7\"/>\n";
        }
    }

    // 5) 点：红色大点 = 当前插入点，蓝色实心 = 已插入，空心 = 未插入
    const bool cleanupFrame = step.insertedPoint == -1 && step.triangles.size() > 1;
    const int insertedCount =
        cleanupFrame ? step.realPointCount
                     : (step.insertedPoint < 0 ? 0 : step.insertedPoint + 1);
    for (int i = 0; i < step.realPointCount; ++i) {
        const std::string x = num(sx(v, step.points[i].x));
        const std::string y = num(sy(v, step.points[i].y));
        if (i == step.insertedPoint) {
            o << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"6\" fill=\"#ef4444\""
              << " stroke=\"#7f1d1d\" stroke-width=\"1.5\"/>\n";
        } else if (i < insertedCount) {
            o << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"3.2\" fill=\"#1e3a8a\"/>\n";
        } else {
            o << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"3.2\" fill=\"none\""
              << " stroke=\"#94a3b8\" stroke-width=\"1.2\"/>\n";
        }
    }

    // 6) 说明文字
    o << "<text x=\"14\" y=\"26\" font-family=\"system-ui, sans-serif\" font-size=\"15\""
      << " fill=\"#0f172a\">第 " << (frameIndex + 1) << " / " << totalFrames
      << " 帧：" << xmlEscape(step.caption) << "</text>\n";
    o << "</svg>\n";
    return o.str();
}

// 把字符串转成 JS 字符串字面量（转义引号、反斜杠、控制字符；< 转成 \u003c 防止 </script> 逃逸）
std::string jsString(const std::string& s) {
    std::ostringstream o;
    o << "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o << "\\\\"; break;
            case '"': o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            case '<': o << "\\u003c"; break;
            default:
                if (c < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<int>(c) << std::dec;
                } else {
                    o << static_cast<char>(c);
                }
        }
    }
    o << "\"";
    return o.str();
}

std::string buildViewerHTML(const std::vector<std::string>& frames) {
    std::ostringstream o;
    o << "<!DOCTYPE html>\n"
      << "<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
      << "<title>2D Delaunay — Bowyer-Watson 逐步演示</title>\n"
      << "<style>\n"
      << "body{font-family:system-ui,'Microsoft YaHei',sans-serif;background:#f1f5f9;"
      << "margin:0;padding:24px;color:#0f172a}\n"
      << ".card{max-width:900px;margin:0 auto;background:#fff;border-radius:12px;"
      << "box-shadow:0 2px 10px rgba(0,0,0,.08);padding:20px 24px}\n"
      << "h1{font-size:20px;margin:0 0 4px}\n"
      << ".sub{color:#64748b;font-size:13px;margin-bottom:14px}\n"
      << ".stage{border:1px solid #e2e8f0;border-radius:8px;overflow:hidden;background:#fff}\n"
      << ".stage svg{display:block;width:100%;height:auto}\n"
      << ".controls{display:flex;gap:10px;align-items:center;margin-top:14px;flex-wrap:wrap}\n"
      << "button{padding:8px 16px;border:1px solid #cbd5e1;border-radius:8px;background:#fff;"
      << "cursor:pointer;font-size:14px}\n"
      << "button:hover{background:#f8fafc}\n"
      << "input[type=range]{flex:1;min-width:180px}\n"
      << ".counter{font-size:13px;color:#475569;min-width:110px;text-align:right}\n"
      << ".legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12px;color:#475569;"
      << "margin-top:12px}\n"
      << ".sw{display:inline-block;width:12px;height:12px;border-radius:3px;"
      << "vertical-align:-1px;margin-right:5px}\n"
      << "</style>\n</head>\n<body>\n<div class=\"card\">\n"
      << "<h1>2D Delaunay 三角剖分 — Bowyer–Watson 增量算法</h1>\n"
      << "<div class=\"sub\">每插入一个点：红色虚线圆 = 违反空圆性质的外接圆，橙色边 = 空腔边界，"
      << "绿色 = 本步新三角形，灰色虚线 = 超三角形相关。</div>\n"
      << "<div class=\"stage\" id=\"stage\"></div>\n"
      << "<div class=\"controls\">\n"
      << "<button id=\"prev\">◀ 上一步</button>\n"
      << "<button id=\"play\">▶ 自动播放</button>\n"
      << "<button id=\"next\">下一步 ▶</button>\n"
      << "<input type=\"range\" id=\"slider\" min=\"0\" value=\"0\">\n"
      << "<span class=\"counter\" id=\"counter\"></span>\n"
      << "</div>\n"
      << "<div class=\"legend\">\n"
      << "<span><span class=\"sw\" style=\"background:#cfe8ff;border:1px solid #2b6cb0\"></span>普通三角形</span>\n"
      << "<span><span class=\"sw\" style=\"background:#b9f6ca;border:1px solid #1b7f3b\"></span>本步新增</span>\n"
      << "<span><span class=\"sw\" style=\"background:#e2e8f0;border:1px solid #94a3b8\"></span>与超三角形相连</span>\n"
      << "<span><span class=\"sw\" style=\"border-top:3px solid #f97316\"></span>空腔边界</span>\n"
      << "<span><span class=\"sw\" style=\"border:2px dashed #ef4444\"></span>被破坏的外接圆</span>\n"
      << "</div>\n</div>\n"
      << "<script>\n"
      << "const frames = [";
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i) o << ",";
        o << jsString(frames[i]);
    }
    o << "];\n"
      << "let idx = 0, timer = null;\n"
      << "const stage = document.getElementById('stage');\n"
      << "const slider = document.getElementById('slider');\n"
      << "const counter = document.getElementById('counter');\n"
      << "slider.max = frames.length - 1;\n"
      << "function show() {\n"
      << "  stage.innerHTML = frames[idx];\n"
      << "  slider.value = idx;\n"
      << "  counter.textContent = (idx + 1) + ' / ' + frames.length + ' 帧';\n"
      << "}\n"
      << "document.getElementById('prev').onclick = () => { idx = Math.max(0, idx - 1); show(); };\n"
      << "document.getElementById('next').onclick = () => { idx = Math.min(frames.length - 1, idx + 1); show(); };\n"
      << "slider.oninput = () => { idx = Number(slider.value); show(); };\n"
      << "document.getElementById('play').onclick = function() {\n"
      << "  if (timer) { clearInterval(timer); timer = null; this.textContent = '▶ 自动播放'; return; }\n"
      << "  this.textContent = '⏸ 暂停';\n"
      << "  timer = setInterval(() => {\n"
      << "    idx++;\n"
      << "    if (idx >= frames.length) { idx = frames.length - 1; clearInterval(timer); timer = null; "
      << "document.getElementById('play').textContent = '▶ 自动播放'; }\n"
      << "    show();\n"
      << "  }, 500);\n"
      << "};\n"
      << "show();\n"
      << "</script>\n"
      << "</body>\n</html>\n";
    return o.str();
}