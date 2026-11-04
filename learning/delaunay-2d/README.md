# 2D Delaunay 三角剖分练习（Bowyer–Watson 增量法）

这是"提升编程能力"对话中约定的第一个动手练习：**从零实现一个 2D Delaunay 三角剖分**，并把它变成看得见的动画。

核心目标不是"会调用库"，而是理解三个问题：

1. 空圆性质到底是什么（Delaunay 三角剖分的定义）；
2. 增量插入时，为什么会出现"被破坏的三角形"；
3. "空腔 + 重连"这个操作如何把新点并入剖分。

## 算法流程

程序实现的是 Bowyer–Watson 增量算法，共五步：

1. **构造超三角形**：用一个足够大的三角形包住所有输入点，作为初始剖分。
2. **逐点插入**：对每个点 p，找出所有外接圆包含 p 的三角形（这些三角形违反空圆性质）。
3. **挖空腔**：删除这些三角形；它们边界上只出现一次的边，围成一个多边形"空腔"。
4. **重连**：把 p 与空腔的每条边界边连成新三角形。
5. **清理**：所有点插入后，删除任何用到超三角形顶点的三角形。

每一步的状态都会保存为一帧，供可视化。

## 目录结构

```text
learning/delaunay-2d/
  README.md
  src/
    delaunay.h / delaunay.cpp   # 算法本体：超三角形、空圆测试、空腔、重连、自检
    svg_render.h / svg_render.cpp  # 把每一帧画成 SVG，并打包成 HTML 查看器
    main.cpp                    # 演示入口：生成点集、运行算法、输出可视化
  output/                       # 运行后生成（frame_000.svg ... 和 viewer.html）
```

## 构建与运行

需要 g++（MinGW-w64，支持 C++17）。在 `learning/delaunay-2d/` 目录下执行：

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -o delaunay2d.exe src/main.cpp src/delaunay.cpp src/svg_render.cpp
.\delaunay2d.exe --points 26 --seed 42 --out output
```

可选参数：`--points N`（点数，默认 26）、`--seed S`（随机种子，默认 42）、`--out DIR`（输出目录，默认 output）。

运行后：

- `output/frame_000.svg` ... `output/frame_027.svg`：每一帧单独一张图；
- `output/viewer.html`：用浏览器打开，可以上一步/下一步/拖动进度条/自动播放。

## 怎么看动画

- 蓝色三角形：当前剖分中的正常三角形；
- 绿色三角形：这一步新生成的三角形；
- 红色虚线圆：这一步被破坏三角形的外接圆——新插入的点正好落在它们里面；
- 橙色粗线：空腔边界（被删除三角形的轮廓），新点与每条橙边重连；
- 灰色虚线：与超三角形相连的三角形（最后一步会被清掉）；
- 实心圆点：已插入的点；红色大点：当前插入的点；空心圆点：还没插入的点。

## 代码导览

算法集中在 `delaunay.cpp`，建议阅读顺序：

1. `incircleDet()`：四阶行列式形式的空圆测试，理解"点在三角形外接圆内"的代数判据；
2. `buildSuperTriangle()`：超三角形的构造；
3. `run()`：主循环——找坏三角形、统计边界边、挖空腔、重连、记录帧；
4. `cleanupSuperTriangle()`：收尾清理；
5. `selfTestReport()`：空圆性质、三角形数量公式 `2n - h - 2`、边一致性三类自检。

## 超三角形的大小（自动重试）

超三角形必须足够大：如果它太小，超三角形顶点可能落在某个真实三角形的外接圆内，导致去掉超三角形后得到的并不是真正的 Delaunay 剖分。程序从 20 倍包围盒开始尝试，若自检不通过会自动用更大的倍数（100、500、2000…）重建，直到通过。这也解释了为什么 `run()` 里自检会作为内部验证来用。

## 自检

程序跑完会打印三组验证：

- **空圆性质**：每个最终三角形的外接圆内不应包含任何其它点；
- **三角形数量**：非退化点集的 Delaunay 三角形数应为 `2n - h - 2`（n 为点数，h 为凸包点数）；
- **边一致性**：内部边被 2 个三角形共享，凸包边被 1 个共享。

## 延伸练习

做完这个基础版之后，可以按难度递进：

1. 改成用半边结构（half-edge/DCEL）存储，练习拓扑操作；
2. 加入约束边（constrained Delaunay），体会"边界恢复"问题的雏形；
3. 用 Shewchuk 的精确谓词替换浮点行列式，观察鲁棒性差异；
4. 把同样的思路推广到 3D（对应 TetGen 的 Delaunay 四面体化与边界恢复）。

## 参考资料

- Bowyer (1981), *Computing Dirichlet tessellations*；Watson (1981), *Computing the n-dimensional Delaunay tessellation with applications to Voronoi polytopes*；
- de Berg 等《Computational Geometry: Algorithms and Applications》第 9 章；
- Jonathan Shewchuk 课程讲义 *Delaunay Mesh Generation*；
- Paul Bourke 的 `triangulate.c`（经典 C 参考实现）。
