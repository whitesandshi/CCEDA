#pragma once

#include "application/MeshingJobController.h"
#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"
#include "domain/MeshingRetryPolicy.h"

#include <QWidget>

class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;
class QWidget;

namespace cceda::ui
{
class MeshingTaskPanel final : public QWidget
{
  Q_OBJECT

public:
  explicit MeshingTaskPanel(QWidget* parent = nullptr);

  void setGeometryAvailable(bool available);
  void setGenerationBlocked(bool blocked);
  void commitSettingsEdits();
  void applySettings(domain::MeshGenerationMode mode,
    const domain::MeshGenerationSettings& settings,
    const domain::MeshingResourceLimits& limits,
    const domain::MeshingRetryPolicy& retry);
  void resetTaskResult();
  void showRestoredMesh(const domain::MeshModel& mesh,
    const application::AcceptedMeshQuality& quality);
  [[nodiscard]] domain::MeshGenerationSettings settings() const noexcept;
  [[nodiscard]] domain::MeshGenerationMode mode() const noexcept;
  [[nodiscard]] domain::MeshingResourceLimits resourceLimits() const noexcept;
  [[nodiscard]] domain::MeshingRetryPolicy retryPolicy() const noexcept;

  void showPreparing();
  void showProgress(const application::MeshingJobProgress& progress);
  void showCancelling();
  void showCancelled(const application::MeshingJobCancellation& cancellation);
  void showSucceeded(const application::ExperimentalMeshingJobResult& result);
  void showFailure(
    const QString& code, const QString& detail, std::uint32_t attemptCount = 0);

signals:
  void generateRequested();
  void cancelRequested();
  void settingsChanged();

private:
  void setBusy(bool busy);
  void requestGeneration();
  void resetDefaults();

  bool geometryAvailable_ = false;
  bool generationBlocked_ = false;
  bool busy_ = false;
  std::optional<double> restoredMinimum_;
  std::optional<double> restoredMaximum_;
  std::optional<std::uint64_t> restoredElapsedLimit_;
  std::optional<std::uint64_t> restoredMemoryLimit_;
  QDoubleSpinBox* minimumSize_ = nullptr;
  QDoubleSpinBox* maximumSize_ = nullptr;
  QComboBox* mode_ = nullptr;
  QToolButton* advancedSettings_ = nullptr;
  QWidget* advancedSettingsPanel_ = nullptr;
  QSpinBox* curvatureElements_ = nullptr;
  QComboBox* algorithm_ = nullptr;
  QCheckBox* optimizeTetrahedra_ = nullptr;
  QCheckBox* automaticRetry_ = nullptr;
  QSpinBox* maximumAttempts_ = nullptr;
  QSpinBox* maximumThreads_ = nullptr;
  QSpinBox* maximumMinutes_ = nullptr;
  QSpinBox* maximumMemoryMiB_ = nullptr;
  QPushButton* resetDefaultsButton_ = nullptr;
  QPushButton* generateButton_ = nullptr;
  QPushButton* cancelButton_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* summary_ = nullptr;
};
}
