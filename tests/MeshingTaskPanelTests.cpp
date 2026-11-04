#include "ui/MeshingTaskPanel.h"

#include <QApplication>
#include <QAccessible>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}

int main(int argc, char* argv[])
{
  QApplication application(argc, argv);
  cceda::ui::MeshingTaskPanel panel;

  auto* generate = panel.findChild<QPushButton*>("generateMeshButton");
  auto* cancel = panel.findChild<QPushButton*>("cancelMeshButton");
  auto* minimum = panel.findChild<QDoubleSpinBox*>("minimumMeshSize");
  auto* maximum = panel.findChild<QDoubleSpinBox*>("maximumMeshSize");
  auto* progress = panel.findChild<QProgressBar*>("meshingProgress");
  auto* advanced = panel.findChild<QToolButton*>("advancedMeshingSettings");
  auto* curvature = panel.findChild<QSpinBox*>("curvatureElementsPerCircle");
  auto* algorithm = panel.findChild<QComboBox*>("meshGenerationAlgorithm");
  auto* optimize = panel.findChild<QCheckBox*>("optimizeTetrahedra");
  auto* automaticRetry = panel.findChild<QCheckBox*>("automaticMeshRetry");
  auto* maximumAttempts = panel.findChild<QSpinBox*>("maximumMeshAttempts");
  auto* threads = panel.findChild<QSpinBox*>("maximumMeshingThreads");
  auto* maximumMinutes = panel.findChild<QSpinBox*>("maximumMeshingMinutes");
  auto* maximumMemory = panel.findChild<QSpinBox*>("maximumMeshingMemoryMiB");
  auto* reset = panel.findChild<QPushButton*>("resetMeshingDefaults");
  auto* status = panel.findChild<QLabel*>("meshingStatus");
  auto* summary = panel.findChild<QLabel*>("meshingSummary");

  expect(generate && cancel && minimum && maximum && progress && status && summary
      && advanced && curvature && algorithm && optimize && automaticRetry
      && maximumAttempts && threads && maximumMinutes
      && maximumMemory && reset,
         "the meshing panel exposes its accessible task controls");
  if (!generate || !cancel || !minimum || !maximum || !progress || !status || !summary
      || !advanced || !curvature || !algorithm || !optimize || !automaticRetry
      || !maximumAttempts || !threads || !maximumMinutes
      || !maximumMemory || !reset)
  {
    return EXIT_FAILURE;
  }

  panel.setGeometryAvailable(false);
  expect(!generate->isEnabled(), "mesh generation is disabled without geometry");
  expect(!cancel->isEnabled(), "cancellation is disabled while idle");

  int generateRequests = 0;
  int cancelRequests = 0;
  QObject::connect(&panel, &cceda::ui::MeshingTaskPanel::generateRequested, &panel,
                   [&] { ++generateRequests; });
  QObject::connect(&panel, &cceda::ui::MeshingTaskPanel::cancelRequested, &panel,
                   [&] { ++cancelRequests; });

  panel.setGeometryAvailable(true);
  expect(!advanced->isChecked() && curvature->value() == 0
      && !optimize->isChecked() && threads->value() == 0
      && algorithm->currentData().toInt()
        == static_cast<int>(cceda::domain::MeshGenerationAlgorithm::Delaunay)
      && !automaticRetry->isChecked() && maximumAttempts->value() == 2
      && !maximumAttempts->isEnabled()
      && maximumMinutes->value() == 0 && maximumMemory->value() == 0,
    "advanced settings start at the versioned defaults");
  advanced->click();
  expect(advanced->isChecked(), "advanced settings can be disclosed");
  minimum->setValue(0.25);
  maximum->setValue(2.0);
  curvature->setValue(24);
  algorithm->setCurrentIndex(1);
  optimize->setChecked(true);
  automaticRetry->setChecked(true);
  maximumAttempts->setValue(2);
  threads->setValue(6);
  maximumMinutes->setValue(15);
  maximumMemory->setValue(4096);
  generate->click();
  expect(generateRequests == 1, "enabled generate control publishes one request");
  const auto configured = panel.settings();
  expect(configured.minimumSizeMm == 0.25 && configured.maximumSizeMm == 2.0
      && configured.curvatureElementsPerCircle == 24
      && configured.optimizeTetrahedra && configured.maximumThreads == 6
      && configured.algorithm == cceda::domain::MeshGenerationAlgorithm::Hxt,
    "the panel exposes the complete typed settings contract");
  const auto configuredRetry = panel.retryPolicy();
  expect(configuredRetry.automaticRetry && configuredRetry.maximumAttempts == 2,
    "the panel exposes the bounded automatic retry policy");
  const auto configuredLimits = panel.resourceLimits();
  expect(configuredLimits.maximumElapsedMilliseconds == 15ULL * 60ULL * 1000ULL
      && configuredLimits.maximumWorkingSetBytes
        == 4096ULL * 1024ULL * 1024ULL,
    "the panel converts resource limits to the domain contract");
  reset->click();
  const auto defaults = panel.settings();
  expect(defaults.minimumSizeMm == 0.0 && defaults.maximumSizeMm == 0.0
      && defaults.curvatureElementsPerCircle == 0
      && !defaults.optimizeTetrahedra && defaults.maximumThreads == 0
      && defaults.algorithm == cceda::domain::MeshGenerationAlgorithm::Delaunay,
    "reset restores every versioned meshing default");
  const auto defaultRetry = panel.retryPolicy();
  expect(!defaultRetry.automaticRetry && defaultRetry.maximumAttempts == 2,
    "reset disables automatic retry and restores its bounded attempt default");
  const auto defaultLimits = panel.resourceLimits();
  expect(defaultLimits.maximumElapsedMilliseconds == 0
      && defaultLimits.maximumWorkingSetBytes == 0,
    "reset disables both explicit resource limits");
  minimum->setValue(3.0);
  maximum->setValue(2.0);
  generate->click();
  expect(generateRequests == 1, "invalid size ordering does not publish a generate request");
  expect(status->text().contains("size", Qt::CaseInsensitive),
         "invalid size ordering produces an inline validation message");

  panel.showPreparing();
  expect(!generate->isEnabled() && cancel->isEnabled(),
         "preparing disables generation and enables cancellation");
  expect(progress->minimum() == 0 && progress->maximum() == 0,
         "preparing uses an indeterminate progress state");

  cceda::application::MeshingJobProgress jobProgress;
  jobProgress.stage = "generating-volume";
  jobProgress.percent = 60;
  jobProgress.elapsedMilliseconds = 1'500;
  jobProgress.workingSetBytes = 64ULL * 1024ULL * 1024ULL;
  jobProgress.attemptNumber = 2;
  jobProgress.maximumAttempts = 2;
  panel.showProgress(jobProgress);
  expect(progress->minimum() == 0 && progress->maximum() == 100 && progress->value() == 60,
         "determinate Worker progress is displayed");
  expect(status->text().contains("volume", Qt::CaseInsensitive),
         "stable Worker stages receive readable UI text");
  expect(summary->text().contains("1.50") && summary->text().contains("64.0")
      && summary->text().contains("2"),
    "live progress exposes attempt, elapsed time, and working set");
  jobProgress.stage = "converting-mesh";
  jobProgress.percent = 50;
  panel.showProgress(jobProgress);
  expect(progress->value() == 60, "out-of-order Worker percentages do not regress progress");
  expect(status->text().contains("convert", Qt::CaseInsensitive),
         "mesh conversion receives readable UI text");

  cancel->click();
  expect(cancelRequests == 1, "enabled cancel control publishes one request");
  panel.showCancelling();
  expect(!cancel->isEnabled(), "repeated cancellation is disabled");

  cceda::application::ExperimentalMeshingJobResult result;
  result.committedMeshRevision = 7;
  result.nodeCount = 100;
  result.triangleCount = 80;
  result.tetrahedronCount = 40;
  result.elapsedMilliseconds = 2'750;
  result.peakWorkingSetBytes = 96ULL * 1024ULL * 1024ULL;
  result.attemptCount = 2;
  panel.showSucceeded(result);
  expect(generate->isEnabled() && !cancel->isEnabled(),
         "successful completion restores the idle controls");
  expect(status->text().contains("ready", Qt::CaseInsensitive),
         "successful completion reports a ready mesh");
  expect(summary->text().contains("7") && summary->text().contains("100") &&
             summary->text().contains("80") && summary->text().contains("40"),
         "success summary exposes revision and element counts");
  expect(summary->text().contains("2.75") && summary->text().contains("96.0")
      && summary->text().contains("2 attempts"),
    "success summary exposes attempts, elapsed time, and peak working set");
  panel.setGenerationBlocked(true);
  expect(!generate->isEnabled() && summary->text().contains("100"),
         "presentation preparation blocks regeneration without clearing the success summary");
  panel.setGenerationBlocked(false);
  expect(generate->isEnabled(), "generation is restored after presentation preparation");
  const auto* statusAccessible = QAccessible::queryAccessibleInterface(status);
  const auto* summaryAccessible = QAccessible::queryAccessibleInterface(summary);
  expect(statusAccessible &&
           statusAccessible->text(QAccessible::Name).contains("ready", Qt::CaseInsensitive),
         "assistive technology receives the dynamic terminal status");
  expect(summaryAccessible && summaryAccessible->text(QAccessible::Name).contains("100"),
         "assistive technology receives the dynamic result summary");

  panel.showFailure("MESH_OUTPUT_CORRUPT", "Verified output changed", 2);
  expect(generate->isEnabled() && !cancel->isEnabled(), "failure restores controls for a retry");
  expect(status->text().contains("failed", Qt::CaseInsensitive) &&
             summary->text().contains("Verified output changed")
             && summary->text().contains("2 attempts"),
         "failure presents a concise terminal state and detail");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
