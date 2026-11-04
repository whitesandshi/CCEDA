#include "delaunay.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace {
constexpr double kEps = 1e-10;
}

Delaunay2D::Delaunay2D(std::vector<Point2> points)
    : points_(std::move(points)), realPoints_(points_) {
    realCount_ = static_cast<int>(realPoints_.size());
}

// 三点叉积：>0 表示逆时针，<0 表示顺时针
double Delaunay2D::orient2d(const Point2& a, const Point2& b, const Point2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// 空圆测试（in-circle 行列式）：
// 对逆时针三角形 (a,b,c)，返回值 > 0 当且仅当 p 严格落在其外接圆内。
// 推导：把 p 平移到原点，检验 a,b,c 是否分布在某个过原点的圆两侧。
double Delaunay2D::incircleDet(const Point2& a, const Point2& b, const Point2& c,
                               const Point2& p) {
    const double adx = a.x - p.x, ady = a.y - p.y;
    const double bdx = b.x - p.x, bdy = b.y - p.y;
    const double cdx = c.x - p.x, cdy = c.y - p.y;
    const double alift = adx * adx + ady * ady;
    const double blift = bdx * bdx + bdy * bdy;
    const double clift = cdx * cdx + cdy * cdy;
    return alift * (bdx * cdy - cdx * bdy)
         - blift * (adx * cdy - cdx * ady)
         + clift * (adx * bdy - bdx * ady);
}

// 三角形外接圆（圆心 + 半径）
Circle Delaunay2D::circumcircleOf(const Point2& a, const Point2& b, const Point2& c) {
    const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    Circle ci;
    if (std::abs(d) < 1e-12) return ci;
    const double a2 = a.x * a.x + a.y * a.y;
    const double b2 = b.x * b.x + b.y * b.y;
    const double c2 = c.x * c.x + c.y * c.y;
    ci.cx = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
    ci.cy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
    ci.r = std::hypot(a.x - ci.cx, a.y - ci.cy);
    return ci;
}

void Delaunay2D::resetState() {
    points_ = realPoints_;
    triangles_.clear();
    finalTriangles_.clear();
    steps_.clear();
    superA_ = superB_ = superC_ = -1;
}

void Delaunay2D::buildSuperTriangle(double multiplier) {
    double minx = realPoints_[0].x, maxx = realPoints_[0].x;
    double miny = realPoints_[0].y, maxy = realPoints_[0].y;
    for (const Point2& p : realPoints_) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    const double dmax = std::max(maxx - minx, maxy - miny);
    const double midx = (minx + maxx) / 2.0;
    const double midy = (miny + maxy) / 2.0;

    realCount_ = static_cast<int>(realPoints_.size());
    superA_ = realCount_;
    superB_ = realCount_ + 1;
    superC_ = realCount_ + 2;
    points_.push_back({midx - multiplier * dmax, midy - dmax});
    points_.push_back({midx + multiplier * dmax, midy - dmax});
    points_.push_back({midx, midy + multiplier * dmax});

    // 超三角形本身必须逆时针，in-circle 行列式的符号约定才成立
    triangles_.push_back(Triangle{superA_, superB_, superC_});
}

bool Delaunay2D::pointInCircumcircle(const Triangle& t, int p) const {
    const Point2& a = points_[t.v[0]];
    const Point2& b = points_[t.v[1]];
    const Point2& c = points_[t.v[2]];
    return incircleDet(a, b, c, points_[p]) > kEps;
}

void Delaunay2D::recordStep(int inserted, std::vector<Circle> removed,
                            std::vector<Edge> cavity, std::vector<Triangle> added,
                            std::string caption) {
    Step s;
    s.points = points_;
    s.realPointCount = realCount_;
    s.triangles = triangles_;
    s.newTriangles = std::move(added);
    s.removedCircles = std::move(removed);
    s.cavityEdges = std::move(cavity);
    s.insertedPoint = inserted;
    s.caption = std::move(caption);
    steps_.push_back(std::move(s));
}

// 依次插入所有真实点；任意一步找不到可破坏的三角形（退化情况）则返回 false
bool Delaunay2D::insertAllPoints() {
    for (int i = 0; i < realCount_; ++i) {
        // 1) 找所有外接圆包含 p 的三角形（违反空圆性质）
        std::vector<int> bad;
        for (int ti = 0; ti < static_cast<int>(triangles_.size()); ++ti) {
            if (pointInCircumcircle(triangles_[ti], i)) bad.push_back(ti);
        }
        if (bad.empty()) return false;

        // 记录被删除三角形的外接圆（可视化用），在删除前计算
        std::vector<Circle> removed;
        removed.reserve(bad.size());
        for (int ti : bad) {
            const Triangle& t = triangles_[ti];
            removed.push_back(circumcircleOf(points_[t.v[0]], points_[t.v[1]], points_[t.v[2]]));
        }

        // 2) 统计坏三角形的边：出现 1 次的边构成空腔边界
        std::map<std::pair<int, int>, int> edgeCount;
        auto normEdge = [](int a, int b) {
            return std::make_pair(std::min(a, b), std::max(a, b));
        };
        for (int ti : bad) {
            const Triangle& t = triangles_[ti];
            for (int k = 0; k < 3; ++k) {
                ++edgeCount[normEdge(t.v[k], t.v[(k + 1) % 3])];
            }
        }
        std::vector<Edge> cavity;
        for (const auto& kv : edgeCount) {
            if (kv.second == 1) cavity.push_back(Edge{kv.first.first, kv.first.second});
        }

        // 3) 删除坏三角形
        std::vector<Triangle> kept;
        kept.reserve(triangles_.size() - bad.size() + cavity.size());
        for (int ti = 0; ti < static_cast<int>(triangles_.size()); ++ti) {
            if (std::find(bad.begin(), bad.end(), ti) == bad.end()) {
                kept.push_back(triangles_[ti]);
            }
        }

        // 4) 把新点与每条空腔边界边连成新三角形，保持逆时针
        std::vector<Triangle> added;
        added.reserve(cavity.size());
        for (const Edge& e : cavity) {
            Triangle nt{e.a, e.b, i};
            if (orient2d(points_[nt.v[0]], points_[nt.v[1]], points_[nt.v[2]]) < 0) {
                std::swap(nt.v[1], nt.v[2]);
            }
            added.push_back(nt);
            kept.push_back(nt);
        }
        triangles_ = std::move(kept);

        std::ostringstream cap;
        cap << "插入点 #" << i << "（" << points_[i].x << ", " << points_[i].y
            << "）：破坏 " << bad.size() << " 个三角形，新增 " << added.size() << " 个";
        recordStep(i, std::move(removed), std::move(cavity), std::move(added), cap.str());
    }
    return true;
}

void Delaunay2D::cleanupSuperTriangle() {
    finalTriangles_.clear();
    for (const Triangle& t : triangles_) {
        const bool usesSuper =
            t.v[0] >= realCount_ || t.v[1] >= realCount_ || t.v[2] >= realCount_;
        if (!usesSuper) finalTriangles_.push_back(t);
    }

    Step s;
    s.points = points_;
    s.realPointCount = realCount_;
    s.triangles = finalTriangles_;
    s.insertedPoint = -1;
    s.caption = "清理：去掉与超三角形相连的三角形，得到最终剖分";
    steps_.push_back(std::move(s));
}

bool Delaunay2D::run() {
    if (realCount_ < 3) return false;
    // 超三角形必须足够大，否则它的顶点可能落在真实三角形外接圆内，
    // 破坏"去掉超三角形后即真实 Delaunay"的性质。从小到大重试，
    // 直到自检通过（保留最后一次成功的帧序列）。
    const double kMultipliers[] = {20.0, 100.0, 500.0, 2000.0, 10000.0, 100000.0};
    for (double mult : kMultipliers) {
        resetState();
        buildSuperTriangle(mult);
        recordStep(-1, {}, {}, {}, "初始化：用超三角形包住所有点");
        if (!insertAllPoints()) continue;
        cleanupSuperTriangle();
        if (isValid()) return true;
    }
    return false;
}

// 单调链凸包（返回凸包顶点下标，逆时针）
std::vector<int> Delaunay2D::convexHull() const {
    std::vector<int> idx(realCount_);
    for (int i = 0; i < realCount_; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int i, int j) {
        return points_[i].x < points_[j].x ||
               (points_[i].x == points_[j].x && points_[i].y < points_[j].y);
    });

    std::vector<int> hull;
    auto cross = [&](int o, int a, int b) {
        return orient2d(points_[o], points_[a], points_[b]);
    };
    for (int i : idx) {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), i) <= 0) {
            hull.pop_back();
        }
        hull.push_back(i);
    }
    const size_t lower = hull.size();
    for (int k = static_cast<int>(idx.size()) - 2; k >= 0; --k) {
        const int i = idx[k];
        while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), i) <= 0) {
            hull.pop_back();
        }
        hull.push_back(i);
    }
    hull.pop_back();
    return hull;
}

bool Delaunay2D::isValid() const {
    // [1] 空圆性质（只检查真实点）
    for (const Triangle& t : finalTriangles_) {
        for (int p = 0; p < realCount_; ++p) {
            if (p == t.v[0] || p == t.v[1] || p == t.v[2]) continue;
            if (pointInCircumcircle(t, p)) return false;
        }
    }
    // [2] 三角形数量：2n - h - 2
    const int expected = 2 * realCount_ - static_cast<int>(convexHull().size()) - 2;
    if (expected != static_cast<int>(finalTriangles_.size())) return false;
    // [3] 边一致性
    std::map<std::pair<int, int>, int> edgeCount;
    for (const Triangle& t : finalTriangles_) {
        for (int k = 0; k < 3; ++k) {
            const int a = t.v[k], b = t.v[(k + 1) % 3];
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            ++edgeCount[key];
        }
    }
    for (const auto& kv : edgeCount) {
        if (kv.second != 1 && kv.second != 2) return false;
    }
    return true;
}

std::string Delaunay2D::selfTestReport() const {
    std::ostringstream os;

    // [1] 空圆性质
    int violations = 0;
    for (const Triangle& t : finalTriangles_) {
        for (int p = 0; p < realCount_; ++p) {
            if (p == t.v[0] || p == t.v[1] || p == t.v[2]) continue;
            if (pointInCircumcircle(t, p)) {
                ++violations;
                if (violations <= 3) {
                    os << "  空圆违规: 三角形 (" << t.v[0] << ", " << t.v[1] << ", " << t.v[2]
                       << ") 的外接圆包含点 " << p << "\n";
                }
            }
        }
    }
    os << "[1] 空圆性质: 每个三角形外接圆不含其它点 -> "
       << (violations == 0 ? "通过" : std::to_string(violations) + " 处违反") << "\n";

    // [2] 三角形数量：2n - h - 2
    const std::vector<int> hull = convexHull();
    const int expected = 2 * realCount_ - static_cast<int>(hull.size()) - 2;
    const int actual = static_cast<int>(finalTriangles_.size());
    os << "[2] 三角形数量: 期望 2n-h-2 = " << expected << "（n=" << realCount_
       << ", h=" << hull.size() << "），实际 " << actual << " -> "
       << (expected == actual ? "一致" : "不一致") << "\n";

    // [3] 边一致性：内部边被 2 个三角形共享，凸包边被 1 个共享
    std::map<std::pair<int, int>, int> edgeCount;
    for (const Triangle& t : finalTriangles_) {
        for (int k = 0; k < 3; ++k) {
            const int a = t.v[k], b = t.v[(k + 1) % 3];
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            ++edgeCount[key];
        }
    }
    int badEdges = 0;
    int boundaryEdges = 0;
    for (const auto& kv : edgeCount) {
        if (kv.second == 1) ++boundaryEdges;
        if (kv.second != 1 && kv.second != 2) ++badEdges;
    }
    os << "[3] 边一致性: 内部边×2、边界边×1，边界边数 " << boundaryEdges
       << "（凸包边 " << hull.size() << "）-> "
       << (badEdges == 0 ? "通过" : std::to_string(badEdges) + " 处异常") << "\n";
    return os.str();
}