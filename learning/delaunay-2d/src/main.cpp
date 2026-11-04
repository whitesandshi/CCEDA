#include "delaunay.h"
#include "svg_render.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// 生成演示点集：两团聚集点 + 若干散布点，便于观察空腔的形成
std::vector<Point2> makeDemoPoints(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<Point2> pts;
    const int cluster = n * 2 / 3;
    for (int i = 0; i < cluster; ++i) {
        const double cx = (i % 2 == 0) ? 30.0 : 70.0;
        const double cy = (i % 2 == 0) ? 35.0 : 45.0;
        const double r = 10.0 * std::sqrt(uni(rng));
        const double th = 2.0 * 3.14159265358979323846 * uni(rng);
        pts.push_back({cx + r * std::cos(th), cy + r * std::sin(th)});
    }
    for (int i = cluster; i < n; ++i) {
        pts.push_back({5.0 + 90.0 * uni(rng), 5.0 + 70.0 * uni(rng)});
    }
    return pts;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}

void printUsage() {
    std::cout << "用法: delaunay2d [--points N] [--seed S] [--out DIR]\n"
              << "  --points N   点数（默认 26）\n"
              << "  --seed S     随机种子（默认 42）\n"
              << "  --out DIR    输出目录（默认 output）\n";
}

}  // namespace

int main(int argc, char** argv) {
    int n = 26;
    unsigned seed = 42;
    std::string outDir = "output";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--points" && i + 1 < argc) {
            n = std::atoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--help") {
            printUsage();
            return 0;
        } else {
            printUsage();
            return 2;
        }
    }
    if (n < 3) {
        std::cerr << "点数至少为 3\n";
        return 2;
    }

    Delaunay2D delaunay(makeDemoPoints(n, seed));
    if (!delaunay.run()) {
        std::cerr << "三角剖分失败（点数不足或全部点共线？）\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    const std::vector<Step>& steps = delaunay.steps();
    std::vector<std::string> frames;
    frames.reserve(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        const std::string svg =
            stepToSVG(steps[i], static_cast<int>(i), static_cast<int>(steps.size()));
        std::ostringstream name;
        name << outDir << "/frame_" << std::setw(3) << std::setfill('0') << i << ".svg";
        if (!writeFile(name.str(), svg)) {
            std::cerr << "无法写入 " << name.str() << "\n";
            return 1;
        }
        frames.push_back(svg);
    }
    if (!writeFile(outDir + "/viewer.html", buildViewerHTML(frames))) {
        std::cerr << "无法写入 " << outDir << "/viewer.html\n";
        return 1;
    }

    std::cout << "===== 运行摘要 =====\n";
    std::cout << "点数: " << n << "\n";
    std::cout << "帧数: " << steps.size()
              << "（初始化 1 + 插入 " << n << " + 清理 1）\n";
    std::cout << "最终三角形数: " << delaunay.finalTriangles().size() << "\n\n";
    std::cout << "===== 自检 =====\n" << delaunay.selfTestReport() << "\n";
    std::cout << "输出: " << outDir << "/viewer.html（" << frames.size()
              << " 张 SVG 帧）\n";
    return 0;
}
