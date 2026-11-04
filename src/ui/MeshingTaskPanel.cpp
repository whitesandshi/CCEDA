#include "ui/MeshingTaskPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace cceda::ui
{
MeshingTaskPanel::MeshingTaskPanel(QWidget* parent) : QWidget(parent)
{
  setObjectName("meshingTaskPanel");
  setAccessibleName(tr("Meshing task"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);

  auto* settings = new QHBoxLayout;
  settings->setSpacing(8);

  auto* minimumLabel = new QLabel(tr("Minimum size:"), this);
  minimumSize_ = new QDoubleSpinBox(this);
  minimumSize_->setObjectName("minimumMeshSize");
  minimumSize_->setAccessibleName(tr("Minimum mesh size"));
  minimumSize_->setRange(0.0, 1'000'000.0);
  minimumSize_->setDecimals(6);
  minimumSize_->setSingleStep(0.1);
  minimumSize_->setSpecialValueText(tr("Automatic"));
  minimumSize_->setSuffix(tr(" mm"));
  minimumSize_->setKeyboardTracking(false);
  minimumLabel->setBuddy(minimumSize_);

  auto* maximumLabel = new QLabel(tr("Maximum size:"), this);
  maximumSize_ = new QDoubleSpinBox(this);
  maximumSize_->setObjectName("maximumMeshSize");
  maximumSize_->setAccessibleName(tr("Maximum mesh size"));
  maximumSize_->setRange(0.0, 1'000'000.0);
  maximumSize_->setDecimals(6);
  maximumSize_->setSingleStep(0.1);
  maximumSize_->setSpecialValueText(tr("Automatic"));
  maximumSize_->setSuffix(tr(" mm"));
  maximumSize_->setKeyboardTracking(false);
  maximumLabel->setBuddy(maximumSize_);

  auto* modeLabel = new QLabel(tr("Mode:"), this);
  mode_ = new QComboBox(this);
  mode_->setObjectName("meshGenerationMode");
  mode_->setAccessibleName(tr("Mesh generation mode"));
  mode_->addItem(tr("Independent solids"),
    static_cast<int>(domain::MeshGenerationMode::IndependentSolids));
  mode_->addItem(tr("Conformal multi-region"),
    static_cast<int>(domain::MeshGenerationMode::ConformalMultiRegion));
  modeLabel->setBuddy(mode_);

  generateButton_ =
      new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("Generate Mesh"), this);
  generateButton_->setObjectName("generateMeshButton");
  generateButton_->setAccessibleName(tr("Generate mesh"));
  generateButton_->setToolTip(tr("Generate a volume mesh for the current geometry"));

  cancelButton_ =
      new QPushButton(style()->standardIcon(QStyle::SP_DialogCancelButton), tr("Cancel"), this);
  cancelButton_->setObjectName("cancelMeshButton");
  cancelButton_->setAccessibleName(tr("Cancel meshing"));
  cancelButton_->setToolTip(tr("Cancel the active meshing task"));

  settings->addWidget(minimumLabel);
  settings->addWidget(minimumSize_);
  settings->addSpacing(8);
  settings->addWidget(maximumLabel);
  settings->addWidget(maximumSize_);
  settings->addSpacing(8);
  settings->addWidget(modeLabel);
  settings->addWidget(mode_);
  settings->addStretch(1);
  settings->addWidget(generateButton_);
  settings->addWidget(cancelButton_);
  layout->addLayout(settings);

  advancedSettings_ = new QToolButton(this);
  advancedSettings_->setObjectName("advancedMeshingSettings");
  advancedSettings_->setText(tr("Advanced"));
  advancedSettings_->setAccessibleName(tr("Advanced meshing settings"));
  advancedSettings_->setCheckable(true);
  advancedSettings_->setArrowType(Qt::RightArrow);
  advancedSettings_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  layout->addWidget(advancedSettings_, 0, Qt::AlignLeft);

  advancedSettingsPanel_ = new QWidget(this);
  auto* advancedLayout = new QGridLayout(advancedSettingsPanel_);
  advancedLayout->setContentsMargins(20, 0, 0, 0);
  advancedLayout->setSpacing(8);

  auto* curvatureLabel = new QLabel(tr("Curvature:"), advancedSettingsPanel_);
  curvatureElements_ = new QSpinBox(advancedSettingsPanel_);
  curvatureElements_->setObjectName("curvatureElementsPerCircle");
  curvatureElements_->setAccessibleName(tr("Curvature elements per circle"));
  curvatureElements_->setRange(
    0, static_cast<int>(domain::MaximumCurvatureElementsPerCircle));
  curvatureElements_->setSpecialValueText(tr("Disabled"));
  curvatureElements_->setSuffix(tr(" / circle"));
  curvatureElements_->setKeyboardTracking(false);
  curvatureLabel->setBuddy(curvatureElements_);

  optimizeTetrahedra_ = new QCheckBox(
    tr("Optimize tetrahedra"), advancedSettingsPanel_);
  optimizeTetrahedra_->setObjectName("optimizeTetrahedra");
  optimizeTetrahedra_->setAccessibleName(tr("Optimize tetrahedra"));

  auto* algorithmLabel = new QLabel(tr("Algorithm:"), advancedSettingsPanel_);
  algorithm_ = new QComboBox(advancedSettingsPanel_);
  algorithm_->setObjectName("meshGenerationAlgorithm");
  algorithm_->setAccessibleName(tr("Volume mesh algorithm"));
  algorithm_->addItem(tr("Delaunay"),
    static_cast<int>(domain::MeshGenerationAlgorithm::Delaunay));
  algorithm_->addItem(tr("HXT"),
    static_cast<int>(domain::MeshGenerationAlgorithm::Hxt));
  algorithmLabel->setBuddy(algorithm_);

  automaticRetry_ = new QCheckBox(
    tr("Automatic retry"), advancedSettingsPanel_);
  automaticRetry_->setObjectName("automaticMeshRetry");
  automaticRetry_->setAccessibleName(tr("Automatic mesh retry"));

  auto* maximumAttemptsLabel = new QLabel(
    tr("Attempts:"), advancedSettingsPanel_);
  maximumAttempts_ = new QSpinBox(advancedSettingsPanel_);
  maximumAttempts_->setObjectName("maximumMeshAttempts");
  maximumAttempts_->setAccessibleName(tr("Maximum mesh attempts"));
  maximumAttempts_->setRange(
    1, static_cast<int>(domain::MaximumAutomaticMeshingAttempts));
  maximumAttempts_->setValue(
    static_cast<int>(domain::MaximumAutomaticMeshingAttempts));
  maximumAttempts_->setEnabled(false);
  maximumAttemptsLabel->setBuddy(maximumAttempts_);

  auto* threadsLabel = new QLabel(tr("Threads:"), advancedSettingsPanel_);
  maximumThreads_ = new QSpinBox(advancedSettingsPanel_);
  maximumThreads_->setObjectName("maximumMeshingThreads");
  maximumThreads_->setAccessibleName(tr("Maximum meshing threads"));
  maximumThreads_->setRange(0, static_cast<int>(domain::MaximumMeshingThreads));
  maximumThreads_->setSpecialValueText(tr("Automatic"));
  maximumThreads_->setKeyboardTracking(false);
  threadsLabel->setBuddy(maximumThreads_);

  auto* maximumMinutesLabel = new QLabel(tr("Time limit:"), advancedSettingsPanel_);
  maximumMinutes_ = new QSpinBox(advancedSettingsPanel_);
  maximumMinutes_->setObjectName("maximumMeshingMinutes");
  maximumMinutes_->setAccessibleName(tr("Maximum meshing time"));
  maximumMinutes_->setRange(0, 24 * 60);
  maximumMinutes_->setSpecialValueText(tr("Disabled"));
  maximumMinutes_->setSuffix(tr(" min"));
  maximumMinutes_->setKeyboardTracking(false);
  maximumMinutesLabel->setBuddy(maximumMinutes_);

  auto* maximumMemoryLabel = new QLabel(
    tr("Memory limit:"), advancedSettingsPanel_);
  maximumMemoryMiB_ = new QSpinBox(advancedSettingsPanel_);
  maximumMemoryMiB_->setObjectName("maximumMeshingMemoryMiB");
  maximumMemoryMiB_->setAccessibleName(tr("Maximum meshing working set"));
  maximumMemoryMiB_->setRange(0, 256 * 1024);
  maximumMemoryMiB_->setSpecialValueText(tr("Disabled"));
  maximumMemoryMiB_->setSuffix(tr(" MiB"));
  maximumMemoryMiB_->setKeyboardTracking(false);
  maximumMemoryLabel->setBuddy(maximumMemoryMiB_);

  resetDefaultsButton_ = new QPushButton(
    style()->standardIcon(QStyle::SP_BrowserReload),
    tr("Reset Defaults"), advancedSettingsPanel_);
  resetDefaultsButton_->setObjectName("resetMeshingDefaults");
  resetDefaultsButton_->setAccessibleName(tr("Reset meshing defaults"));

  advancedLayout->addWidget(curvatureLabel, 0, 0);
  advancedLayout->addWidget(curvatureElements_, 0, 1);
  advancedLayout->addWidget(optimizeTetrahedra_, 0, 2);
  advancedLayout->addWidget(threadsLabel, 0, 3);
  advancedLayout->addWidget(maximumThreads_, 0, 4);
  advancedLayout->addWidget(algorithmLabel, 1, 0);
  advancedLayout->addWidget(algorithm_, 1, 1);
  advancedLayout->addWidget(automaticRetry_, 1, 2);
  advancedLayout->addWidget(maximumAttemptsLabel, 1, 3);
  advancedLayout->addWidget(maximumAttempts_, 1, 4);
  advancedLayout->addWidget(maximumMinutesLabel, 2, 0);
  advancedLayout->addWidget(maximumMinutes_, 2, 1);
  advancedLayout->addWidget(maximumMemoryLabel, 2, 2);
  advancedLayout->addWidget(maximumMemoryMiB_, 2, 3, 1, 2);
  advancedLayout->setColumnStretch(5, 1);
  advancedLayout->addWidget(resetDefaultsButton_, 0, 6, 3, 1);
  advancedSettingsPanel_->setVisible(false);
  layout->addWidget(advancedSettingsPanel_);

  progress_ = new QProgressBar(this);
  progress_->setObjectName("meshingProgress");
  progress_->setAccessibleName(tr("Meshing progress"));
  progress_->setRange(0, 100);
  progress_->setValue(0);
  progress_->setTextVisible(true);
  layout->addWidget(progress_);

  status_ = new QLabel(tr("Import a meshable STEP model"), this);
  status_->setObjectName("meshingStatus");
  summary_ = new QLabel(this);
  summary_->setObjectName("meshingSummary");
  summary_->setWordWrap(true);
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  layout->addWidget(status_);
  layout->addWidget(summary_);

  connect(generateButton_, &QPushButton::clicked, this, [this] { requestGeneration(); });
  connect(cancelButton_, &QPushButton::clicked, this, &MeshingTaskPanel::cancelRequested);
  connect(advancedSettings_, &QToolButton::toggled, this, [this](bool expanded) {
    advancedSettings_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    advancedSettingsPanel_->setVisible(expanded);
  });
  connect(resetDefaultsButton_, &QPushButton::clicked,
    this, [this] { resetDefaults(); });
  connect(automaticRetry_, &QCheckBox::toggled,
    maximumAttempts_, &QSpinBox::setEnabled);
  setBusy(false);
}

void MeshingTaskPanel::setGeometryAvailable(bool available)
{
  geometryAvailable_ = available;
  if (!busy_)
  {
    generateButton_->setEnabled(geometryAvailable_ && !generationBlocked_);
    status_->setText(geometryAvailable_ ? tr("Ready to generate mesh")
                                        : tr("Import a meshable STEP model"));
    if (!geometryAvailable_)
    {
      summary_->clear();
    }
  }
}

void MeshingTaskPanel::setGenerationBlocked(bool blocked)
{
  generationBlocked_ = blocked;
  if (!busy_)
  {
    generateButton_->setEnabled(geometryAvailable_ && !generationBlocked_);
  }
}

domain::MeshGenerationSettings MeshingTaskPanel::settings() const noexcept
{
  return {
    minimumSize_->value(),
    maximumSize_->value(),
    static_cast<std::uint32_t>(curvatureElements_->value()),
    optimizeTetrahedra_->isChecked(),
    static_cast<std::uint32_t>(maximumThreads_->value()),
    static_cast<domain::MeshGenerationAlgorithm>(
      algorithm_->currentData().toInt()),
  };
}

domain::MeshGenerationMode MeshingTaskPanel::mode() const noexcept
{
  const int value = mode_->currentData().toInt();
  return value == static_cast<int>(domain::MeshGenerationMode::ConformalMultiRegion)
    ? domain::MeshGenerationMode::ConformalMultiRegion
    : domain::MeshGenerationMode::IndependentSolids;
}

domain::MeshingResourceLimits MeshingTaskPanel::resourceLimits() const noexcept
{
  return {
    static_cast<std::uint64_t>(maximumMinutes_->value()) * 60ULL * 1000ULL,
    static_cast<std::uint64_t>(maximumMemoryMiB_->value()) * 1024ULL * 1024ULL,
  };
}

domain::MeshingRetryPolicy MeshingTaskPanel::retryPolicy() const noexcept
{
  return {
    automaticRetry_->isChecked(),
    static_cast<std::uint32_t>(maximumAttempts_->value()),
  };
}

void MeshingTaskPanel::setBusy(bool busy)
{
  busy_ = busy;
  minimumSize_->setEnabled(!busy_);
  maximumSize_->setEnabled(!busy_);
  mode_->setEnabled(!busy_);
  advancedSettings_->setEnabled(!busy_);
  advancedSettingsPanel_->setEnabled(!busy_);
  generateButton_->setEnabled(!busy_ && geometryAvailable_ && !generationBlocked_);
  cancelButton_->setEnabled(busy_);
}

void MeshingTaskPanel::requestGeneration()
{
  if (!domain::isValidMeshGenerationSettings(settings()))
  {
    status_->setText(tr("Check mesh size settings"));
    summary_->setText(tr("Minimum size cannot exceed maximum size."));
    minimumSize_->setFocus(Qt::OtherFocusReason);
    return;
  }
  emit generateRequested();
}

void MeshingTaskPanel::resetDefaults()
{
  minimumSize_->setValue(0.0);
  maximumSize_->setValue(0.0);
  mode_->setCurrentIndex(0);
  curvatureElements_->setValue(0);
  algorithm_->setCurrentIndex(0);
  optimizeTetrahedra_->setChecked(false);
  automaticRetry_->setChecked(false);
  maximumAttempts_->setValue(
    static_cast<int>(domain::MaximumAutomaticMeshingAttempts));
  maximumThreads_->setValue(0);
  maximumMinutes_->setValue(0);
  maximumMemoryMiB_->setValue(0);
}
}
