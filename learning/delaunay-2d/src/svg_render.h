#pragma once

#include "delaunay.h"

#include <string>
#include <vector>

// 把一帧画成独立 SVG 字符串
std::string stepToSVG(const Step& step, int frameIndex, int totalFrames);

// 把所有帧打包成一个可交互的 HTML 查看器（上一步/下一步/进度条/自动播放）
std::string buildViewerHTML(const std::vector<std::string>& frames);
