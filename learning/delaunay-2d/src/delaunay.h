#pragma once

#include <string>
#include <vector>

// 平面点
struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

// 三角形：三个顶点在 points 数组中的下标（约定逆时针排列）
struct Triangle {
    int v[3] = {-1, -1, -1};
};

// 无向边
struct Edge {
    int a = -1;
    int b = -1;
};

// 圆（用于可视化被破坏三角形的外接圆）
struct Circle {
    double cx = 0.0;
    double cy = 0.0;
    double r = 0.0;
};

// 一帧快照：完成某次插入（或初始化/清理）后的完整状态
struct Step {
    std::vector<Point2> points;        // 全部点：真实点在前，超三角形 3 个顶点在后
    int realPointCount = 0;            // 真实点的数量
    std::vector<Triangle> triangles;   // 当前三角形集合（可能含超三角形相关三角形）
    std::vector<Triangle> newTriangles;     // 本步新增的三角形
    std::vector<Circle> removedCircles;     // 本步被删除三角形的外接圆
    std::vector<Edge> cavityEdges;          // 本步空腔的边界边
    int insertedPoint = -1;                 // 本步插入的点下标（-1 表示初始化/清理帧）
    std::string caption;                    // 说明文字
};

// Bowyer-Watson 增量 Delaunay 三角剖分
class Delaunay2D {
public:
    explicit Delaunay2D(std::vector<Point2> points);

    // 执行全部插入并记录每一帧；若失败（点数不足或全部点共线）返回 false
    bool run();
    const std::vector<Step>& steps() const { return steps_; }
    const std::vector<Triangle>& finalTriangles() const { return finalTriangles_; }
    const std::vector<Point2>& points() const { return points_; }

    std::string selfTestReport() const;          // 空圆性质 / 三角形数量 / 边一致性自检

private:
    std::vector<Point2> points_;        // 全部点：真实点 + 超三角形顶点（构建后）
    std::vector<Point2> realPoints_;    // 仅真实点（用于重置重试）
    std::vector<Triangle> triangles_;
    std::vector<Triangle> finalTriangles_;
    std::vector<Step> steps_;
    int realCount_ = 0;
    int superA_ = -1, superB_ = -1, superC_ = -1;

    void buildSuperTriangle(double multiplier);
    bool insertAllPoints();
    void resetState();
    static double orient2d(const Point2& a, const Point2& b, const Point2& c);
    static double incircleDet(const Point2& a, const Point2& b, const Point2& c, const Point2& p);
    static Circle circumcircleOf(const Point2& a, const Point2& b, const Point2& c);
    bool pointInCircumcircle(const Triangle& t, int p) const;
    void recordStep(int inserted, std::vector<Circle> removed,
                    std::vector<Edge> cavity, std::vector<Triangle> added,
                    std::string caption);
    void cleanupSuperTriangle();
    std::vector<int> convexHull() const;
    bool isValid() const;                        // 供 run() 内部验证
};