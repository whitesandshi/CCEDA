#include "geometry/IGeometryValidationService.h"
#include "ui/GeometryValidationPanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QSemaphore>
#include <QTableView>
#include <QThread>

#include <atomic>
#include <iostream>

namespace
{
using namespace cceda::domain;

class ValidationService final : public cceda::geometry::IGeometryValidationService
{
public:
  SolidBrepCheckResult checkSolidBrep(GeometryRevision, const std::string&) const override
  {
    return {};
  }

  GeometryValidationReport checkDocumentBrep(GeometryRevision revision) const override
  {
    if (++calls == 1)
    {
      firstStarted.release();
      firstMayFinish.acquire();
      return {revision, GeometryValidationReportStatus::Completed,
              {{revision, "old-solid", SolidBrepCheckStatus::Valid}}};
    }
    if (revision == 8)
    {
      return {7, GeometryValidationReportStatus::Completed,
              {{7, "stale-solid", SolidBrepCheckStatus::Valid}}};
    }
    return {revision, GeometryValidationReportStatus::Completed,
            {{revision, "current-solid", SolidBrepCheckStatus::Invalid,
              GeometryIssue{"issue", "GEO_INVALID_BREP"}},
             {revision, "unavailable-solid", SolidBrepCheckStatus::CheckUnavailable}}};
  }

  mutable std::atomic<int> calls{0};
  mutable QSemaphore firstStarted;
  mutable QSemaphore firstMayFinish;
};

template<class Predicate>
bool waitFor(Predicate ready)
{
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 5000)
  {
    QApplication::processEvents();
    QThread::msleep(1);
  }
  return ready();
}
}

int main(int argc, char* argv[])
{
  QApplication app(argc, argv);
  auto service = std::make_shared<ValidationService>();
  cceda::ui::GeometryValidationPanel panel(service);
  auto* status = panel.findChild<QLabel*>("geometryValidationStatus");
  auto* table = panel.findChild<QTableView*>("geometryValidationTable");
  if (!status || !table || table->model()->rowCount() != 0)
  {
    std::cerr << "The panel must start with an empty report\n";
    return 1;
  }

  bool showedOldReport = false;
  QObject::connect(table->model(), &QAbstractItemModel::modelReset, &panel, [&] {
    showedOldReport |= table->model()->index(0, 0).data().toString() == "old-solid";
  });
  panel.checkGeometry(7);
  const bool firstStarted = service->firstStarted.tryAcquire(1, 5000);
  panel.checkGeometry(7);
  service->firstMayFinish.release();
  if (!firstStarted || !waitFor([&] { return table->model()->rowCount() == 2; }) ||
      showedOldReport || service->calls != 2 ||
      table->model()->index(0, 0).data().toString() != "current-solid" ||
      table->model()->index(0, 2).data().toString() != "GEO_INVALID_BREP" ||
      table->model()->index(1, 1).data().toString() != "Check unavailable")
  {
    std::cerr << "Only the latest project activation may publish its Solid outcomes\n";
    return 1;
  }

  bool located = false;
  QObject::connect(&panel, &cceda::ui::GeometryValidationPanel::solidActivated, &panel,
                   [&](GeometryRevision revision, const std::string& id) {
                     located = revision == 7 && id == "current-solid";
                   });
  table->activated(table->model()->index(0, 0));
  if (!located)
  {
    std::cerr << "Activating a row must identify the Solid and its report revision\n";
    return 1;
  }

  panel.checkGeometry(8);
  if (table->model()->rowCount() != 0 ||
      !waitFor([&] { return status->text().contains("changed"); }) ||
      table->model()->rowCount() != 0)
  {
    std::cerr << "A mismatched result must leave the old report cleared\n";
    return 1;
  }
  panel.checkGeometry(0);
  return status->text() == "No geometry loaded" && table->model()->rowCount() == 0
    ? 0 : 1;
}
