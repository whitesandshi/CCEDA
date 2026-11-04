#pragma once

#include "application/MeshExportTransaction.h"

#include <QFutureWatcher>
#include <QObject>

#include <filesystem>
#include <memory>
#include <optional>

namespace cceda::application
{
class MeshExportJobController final : public QObject
{
  Q_OBJECT

public:
  MeshExportJobController(
    domain::ProjectState& project,
    std::shared_ptr<IMeshExporter> exporter,
    std::shared_ptr<IStagedFilePublisher> publisher,
    QObject* parent = nullptr);
  ~MeshExportJobController() override;

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] std::optional<MeshExportResult> start(
    const domain::MeshExportSettings& settings,
    const std::filesystem::path& finalPath);

signals:
  void runningChanged(bool running);
  void finished(const cceda::application::MeshExportResult& result);

private:
  void stagingFinished();
  void complete(MeshExportResult result);

  domain::ProjectState& project_;
  std::shared_ptr<IMeshExporter> exporter_;
  std::shared_ptr<IStagedFilePublisher> publisher_;
  QFutureWatcher<std::shared_ptr<MeshExportStageOutcome>> stagingWatcher_;
  bool running_ = false;
};
}
