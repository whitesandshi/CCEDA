#include "ui/MeshingTaskPanel.h"

#include <QCoreApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>

namespace
{
QString stageText(const QString& stage)
{
  const auto text = [](const char* value) {
    return QCoreApplication::translate("MeshingTaskPanel", value);
  };
  if (stage == "initializing")
    return text("Initializing");
  if (stage == "loading-geometry")
    return text("Loading geometry");
  if (stage == "preparing-topology")
    return text("Preparing topology");
  if (stage == "generating-surface")
    return text("Generating surface mesh");
  if (stage == "validating-surface")
    return text("Validating surface mesh");
  if (stage == "generating-volume")
    return text("Generating volume mesh");
  if (stage == "converting-mesh")
    return text("Converting mesh");
  if (stage == "optimizing")
    return text("Optimizing mesh");
  if (stage == "evaluating-quality")
    return text("Evaluating quality");
  if (stage == "writing-result")
    return text("Verifying result");
  if (stage == "retrying")
    return text("Preparing fallback attempt");
  if (stage == "completed")
    return text("Finalizing");
  return text("Meshing");
}

QString secondsText(std::uint64_t elapsedMilliseconds)
{
  return QString::number(
    static_cast<double>(elapsedMilliseconds) / 1000.0, 'f', 2);
}

QString mebibytesText(std::uint64_t bytes)
{
  constexpr double BytesPerMebibyte = 1024.0 * 1024.0;
  return QString::number(static_cast<double>(bytes) / BytesPerMebibyte, 'f', 1);
}
}

namespace cceda::ui
{
void MeshingTaskPanel::showPreparing()
{
  setBusy(true);
  progress_->setRange(0, 0);
  status_->setText(tr("Preparing geometry snapshot"));
  summary_->clear();
}

void MeshingTaskPanel::showProgress(const application::MeshingJobProgress& progress)
{
  setBusy(true);
  if (progress.percent)
  {
    const int percent = std::clamp(*progress.percent, 0, 100);
    const int displayedPercent = progress_->maximum() > progress_->minimum()
                                     ? std::max(progress_->value(), percent)
                                     : percent;
    progress_->setRange(0, 100);
    progress_->setValue(displayedPercent);
  }
  else
  {
    progress_->setRange(0, 0);
  }
  status_->setText(stageText(progress.stage));
  summary_->setText(tr("Attempt %1 of %2 | Elapsed %3 s | Memory %4 MiB")
    .arg(progress.attemptNumber)
    .arg(progress.maximumAttempts)
    .arg(secondsText(progress.elapsedMilliseconds),
      mebibytesText(progress.workingSetBytes)));
}

void MeshingTaskPanel::showCancelling()
{
  setBusy(true);
  cancelButton_->setEnabled(false);
  status_->setText(tr("Cancelling meshing"));
}

void MeshingTaskPanel::showCancelled(const application::MeshingJobCancellation& cancellation)
{
  setBusy(false);
  progress_->setRange(0, 100);
  progress_->setValue(0);
  status_->setText(tr("Meshing cancelled"));
  summary_->setText(cancellation.reason);
}

void MeshingTaskPanel::showSucceeded(const application::ExperimentalMeshingJobResult& result)
{
  setBusy(false);
  progress_->setRange(0, 100);
  progress_->setValue(100);
  status_->setText(tr("Mesh ready"));
  summary_->setText(tr("Revision %1 | %2 nodes | %3 triangles | %4 tetrahedra "
                       "| Quality valid | %5 s | Peak %6 MiB | %7 attempts")
                        .arg(result.committedMeshRevision)
                        .arg(result.nodeCount)
                        .arg(result.triangleCount)
                        .arg(result.tetrahedronCount)
                        .arg(secondsText(result.elapsedMilliseconds),
                          mebibytesText(result.peakWorkingSetBytes))
                        .arg(result.attemptCount));
}

void MeshingTaskPanel::showFailure(
  const QString& code, const QString& detail, std::uint32_t attemptCount)
{
  setBusy(false);
  progress_->setRange(0, 100);
  progress_->setValue(0);
  status_->setText(tr("Meshing failed"));
  const QString message = detail.isEmpty()
    ? tr("See the task log for details") : detail;
  summary_->setText(attemptCount > 0
    ? tr("%1 attempts | %2").arg(attemptCount).arg(message)
    : message);
  summary_->setToolTip(code);
}
}
