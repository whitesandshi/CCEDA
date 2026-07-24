#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_TypeOfVisualization.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kMainClassName[] = L"CCEDA.MainWindow";
constexpr wchar_t kViewClassName[] = L"CCEDA.Viewport";

enum CommandId : UINT
{
  CommandOpen = 1001,
  CommandFit,
  CommandIsometric,
  CommandClear,
  CommandExit,
  CommandAbout,
};

std::wstring Utf8ToWide(const char* theText)
{
  if (theText == nullptr || *theText == '\0')
  {
    return {};
  }

  int aSize = MultiByteToWideChar(CP_UTF8, 0, theText, -1, nullptr, 0);
  UINT aCodePage = CP_UTF8;
  if (aSize == 0)
  {
    aCodePage = CP_ACP;
    aSize = MultiByteToWideChar(aCodePage, 0, theText, -1, nullptr, 0);
  }
  if (aSize == 0)
  {
    return L"Unknown error";
  }

  std::wstring aResult(static_cast<size_t>(aSize), L'\0');
  MultiByteToWideChar(aCodePage, 0, theText, -1, aResult.data(), aSize);
  aResult.resize(static_cast<size_t>(aSize - 1));
  return aResult;
}

std::string WideToUtf8(const std::wstring& theText)
{
  if (theText.empty())
  {
    return {};
  }

  const int aSize = WideCharToMultiByte(
    CP_UTF8, 0, theText.c_str(), static_cast<int>(theText.size()), nullptr, 0, nullptr, nullptr);
  if (aSize == 0)
  {
    return {};
  }

  std::string aResult(static_cast<size_t>(aSize), '\0');
  WideCharToMultiByte(CP_UTF8,
                      0,
                      theText.c_str(),
                      static_cast<int>(theText.size()),
                      aResult.data(),
                      aSize,
                      nullptr,
                      nullptr);
  return aResult;
}

std::wstring FileNameFromPath(const std::wstring& thePath)
{
  const size_t aPos = thePath.find_last_of(L"\\/");
  return aPos == std::wstring::npos ? thePath : thePath.substr(aPos + 1);
}

bool IsStepPath(const std::wstring& thePath)
{
  const size_t aPos = thePath.find_last_of(L'.');
  if (aPos == std::wstring::npos)
  {
    return false;
  }
  std::wstring anExtension = thePath.substr(aPos);
  std::transform(anExtension.begin(),
                 anExtension.end(),
                 anExtension.begin(),
                 [](wchar_t theChar) { return static_cast<wchar_t>(std::towlower(theChar)); });
  return anExtension == L".step" || anExtension == L".stp";
}

void ConfigureOcctResources()
{
  const std::wstring aRoot = CCEDA_OCC_RESOURCE_DIR;
  const auto setPath = [&aRoot](const wchar_t* theName, const wchar_t* theRelativePath)
  {
    SetEnvironmentVariableW(theName, (aRoot + L"/" + theRelativePath).c_str());
  };

  SetEnvironmentVariableW(L"CSF_LANGUAGE", L"us");
  setPath(L"CSF_SHMessage", L"SHMessage");
  setPath(L"CSF_XSMessage", L"XSMessage");
  setPath(L"CSF_STEPDefaults", L"XSTEPResource");
  setPath(L"CSF_ShadersDirectory", L"Shaders");
  setPath(L"CSF_MDTVTexturesDirectory", L"Textures");
}
}

class StepViewerApp
{
public:
  explicit StepViewerApp(HINSTANCE theInstance)
      : myInstance(theInstance)
  {
  }

  bool Create(int theShowCommand)
  {
    if (!RegisterWindowClasses())
    {
      return false;
    }

    myMainWindow = CreateWindowExW(0,
                                   kMainClassName,
                                   L"CCEDA STEP Viewer",
                                   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   1200,
                                   800,
                                   nullptr,
                                   nullptr,
                                   myInstance,
                                   this);
    if (myMainWindow == nullptr)
    {
      return false;
    }
    ShowWindow(myMainWindow, theShowCommand);
    UpdateWindow(myMainWindow);
    return true;
  }

  int Run()
  {
    ACCEL anAccelerators[] = {
      {FVIRTKEY | FCONTROL, static_cast<WORD>('O'), CommandOpen},
      {FVIRTKEY, static_cast<WORD>('F'), CommandFit},
      {FVIRTKEY, static_cast<WORD>('0'), CommandIsometric},
      {FVIRTKEY, VK_DELETE, CommandClear},
    };
    HACCEL anAcceleratorTable =
      CreateAcceleratorTableW(anAccelerators, ARRAYSIZE(anAccelerators));

    MSG aMessage = {};
    while (GetMessageW(&aMessage, nullptr, 0, 0) > 0)
    {
      if (anAcceleratorTable == nullptr
          || !TranslateAcceleratorW(myMainWindow, anAcceleratorTable, &aMessage))
      {
        TranslateMessage(&aMessage);
        DispatchMessageW(&aMessage);
      }
    }
    if (anAcceleratorTable != nullptr)
    {
      DestroyAcceleratorTable(anAcceleratorTable);
    }
    return static_cast<int>(aMessage.wParam);
  }

  bool ImportStep(const std::wstring& thePath)
  {
    if (myContext.IsNull() || !IsStepPath(thePath))
    {
      ShowError(L"Select a .step or .stp file.");
      return false;
    }

    SetStatus(L"Reading " + FileNameFromPath(thePath) + L"...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    try
    {
      STEPControl_Reader aReader;
      const std::string aUtf8Path = WideToUtf8(thePath);
      if (aUtf8Path.empty() || aReader.ReadFile(aUtf8Path.c_str()) != IFSelect_RetDone)
      {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        ShowError(L"Open CASCADE could not read this STEP file.");
        return false;
      }
      if (!aReader.TransferRoots())
      {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        ShowError(L"The STEP file was read, but no model could be transferred.");
        return false;
      }

      const TopoDS_Shape aShape = aReader.OneShape();
      if (aShape.IsNull())
      {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        ShowError(L"The STEP file contains no displayable shape.");
        return false;
      }

      myContext->RemoveAll(false);
      occ::handle<AIS_Shape> aPresentation = new AIS_Shape(aShape);
      aPresentation->SetColor(Quantity_Color(0.72, 0.76, 0.82, Quantity_TOC_sRGB));
      myContext->Display(aPresentation, AIS_Shaded, 0, false);
      myView->FitAll(0.01, false);
      myView->ZFitAll();
      myView->Redraw();

      myCurrentPath = thePath;
      const std::wstring aFileName = FileNameFromPath(thePath);
      SetWindowTextW(myMainWindow, (aFileName + L" - CCEDA STEP Viewer").c_str());
      SetStatus(L"Loaded " + aFileName + L" (" + std::to_wstring(aReader.NbShapes())
                + L" translated shape(s))");
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      return true;
    }
    catch (const Standard_Failure& theFailure)
    {
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      ShowError(L"Open CASCADE error: " + Utf8ToWide(theFailure.GetMessageString()));
      return false;
    }
    catch (...)
    {
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      ShowError(L"An unexpected error occurred while importing the STEP file.");
      return false;
    }
  }

private:
  bool RegisterWindowClasses()
  {
    WNDCLASSEXW aMainClass = {};
    aMainClass.cbSize = sizeof(aMainClass);
    aMainClass.style = CS_HREDRAW | CS_VREDRAW;
    aMainClass.lpfnWndProc = MainWindowProc;
    aMainClass.hInstance = myInstance;
    aMainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    aMainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    aMainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    aMainClass.lpszClassName = kMainClassName;
    aMainClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW aViewClass = {};
    aViewClass.cbSize = sizeof(aViewClass);
    aViewClass.style = CS_OWNDC;
    aViewClass.lpfnWndProc = ViewWindowProc;
    aViewClass.hInstance = myInstance;
    aViewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    aViewClass.lpszClassName = kViewClassName;
    return RegisterClassExW(&aMainClass) != 0 && RegisterClassExW(&aViewClass) != 0;
  }

  bool OnCreate()
  {
    SetMenu(myMainWindow, CreateApplicationMenu());
    myToolbar = CreateWindowExW(0,
                                TOOLBARCLASSNAMEW,
                                nullptr,
                                WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
                                0,
                                0,
                                0,
                                0,
                                myMainWindow,
                                nullptr,
                                myInstance,
                                nullptr);
    myStatusBar = CreateWindowExW(0,
                                  STATUSCLASSNAMEW,
                                  L"Ready",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0,
                                  0,
                                  0,
                                  0,
                                  myMainWindow,
                                  nullptr,
                                  myInstance,
                                  nullptr);
    myViewWindow = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   kViewClassName,
                                   nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                   0,
                                   0,
                                   100,
                                   100,
                                   myMainWindow,
                                   nullptr,
                                   myInstance,
                                   this);
    if (myToolbar == nullptr || myStatusBar == nullptr || myViewWindow == nullptr)
    {
      return false;
    }

    InitializeToolbar();
    DragAcceptFiles(myViewWindow, TRUE);
    LayoutChildren();
    try
    {
      myDisplayConnection = new Aspect_DisplayConnection();
      myGraphicDriver = new OpenGl_GraphicDriver(myDisplayConnection);
      myViewer = new V3d_Viewer(myGraphicDriver);
      myViewer->SetDefaultLights();
      myViewer->SetLightOn();

      myView = myViewer->CreateView();
      myOcctWindow = new WNT_Window(reinterpret_cast<Aspect_Handle>(myViewWindow));
      myView->SetWindow(myOcctWindow);
      myView->SetBackgroundColor(Quantity_Color(0.91, 0.93, 0.95, Quantity_TOC_sRGB));
      myView->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
      myView->TriedronDisplay(
        Aspect_TOTP_LEFT_LOWER, Quantity_NOC_GRAY40, 0.08, V3d_ZBUFFER);
      if (!myOcctWindow->IsMapped())
      {
        myOcctWindow->Map();
      }

      myContext = new AIS_InteractiveContext(myViewer);
      myContext->SetDisplayMode(AIS_Shaded, false);
      myView->MustBeResized();
      myView->Redraw();
    }
    catch (const Standard_Failure& theFailure)
    {
      ShowError(L"Could not initialize the Open CASCADE viewer: "
                + Utf8ToWide(theFailure.GetMessageString()));
      return false;
    }
    return true;
  }

  HMENU CreateApplicationMenu() const
  {
    HMENU aMenuBar = CreateMenu();
    HMENU aFileMenu = CreatePopupMenu();
    HMENU aViewMenu = CreatePopupMenu();
    HMENU aHelpMenu = CreatePopupMenu();
    AppendMenuW(aFileMenu, MF_STRING, CommandOpen, L"&Open STEP...\tCtrl+O");
    AppendMenuW(aFileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(aFileMenu, MF_STRING, CommandExit, L"E&xit");
    AppendMenuW(aViewMenu, MF_STRING, CommandFit, L"&Fit all\tF");
    AppendMenuW(aViewMenu, MF_STRING, CommandIsometric, L"&Isometric\t0");
    AppendMenuW(aViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(aViewMenu, MF_STRING, CommandClear, L"&Clear\tDelete");
    AppendMenuW(aHelpMenu, MF_STRING, CommandAbout, L"&About");
    AppendMenuW(aMenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(aFileMenu), L"&File");
    AppendMenuW(aMenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(aViewMenu), L"&View");
    AppendMenuW(aMenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(aHelpMenu), L"&Help");
    return aMenuBar;
  }

  void InitializeToolbar()
  {
    SendMessageW(myToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(
      myToolbar, TB_LOADIMAGES, IDB_STD_SMALL_COLOR, reinterpret_cast<LPARAM>(HINST_COMMCTRL));

    TBBUTTON aButtons[5] = {};
    aButtons[0].iBitmap = STD_FILEOPEN;
    aButtons[0].idCommand = CommandOpen;
    aButtons[0].fsState = TBSTATE_ENABLED;
    aButtons[0].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    aButtons[0].iString = reinterpret_cast<INT_PTR>(L"Open STEP");
    aButtons[1].iBitmap = STD_FIND;
    aButtons[1].idCommand = CommandFit;
    aButtons[1].fsState = TBSTATE_ENABLED;
    aButtons[1].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    aButtons[1].iString = reinterpret_cast<INT_PTR>(L"Fit all");
    aButtons[2].iBitmap = STD_PROPERTIES;
    aButtons[2].idCommand = CommandIsometric;
    aButtons[2].fsState = TBSTATE_ENABLED;
    aButtons[2].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    aButtons[2].iString = reinterpret_cast<INT_PTR>(L"Isometric");
    aButtons[3].fsStyle = BTNS_SEP;
    aButtons[4].iBitmap = STD_DELETE;
    aButtons[4].idCommand = CommandClear;
    aButtons[4].fsState = TBSTATE_ENABLED;
    aButtons[4].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    aButtons[4].iString = reinterpret_cast<INT_PTR>(L"Clear");
    SendMessageW(myToolbar, TB_ADDBUTTONS, ARRAYSIZE(aButtons), reinterpret_cast<LPARAM>(aButtons));
    SendMessageW(myToolbar, TB_AUTOSIZE, 0, 0);
  }

  void LayoutChildren()
  {
    if (myMainWindow == nullptr)
    {
      return;
    }
    RECT aClient = {};
    GetClientRect(myMainWindow, &aClient);

    int aToolbarHeight = 0;
    if (myToolbar != nullptr)
    {
      SendMessageW(myToolbar, TB_AUTOSIZE, 0, 0);
      RECT aRect = {};
      GetWindowRect(myToolbar, &aRect);
      aToolbarHeight = aRect.bottom - aRect.top;
    }
    int aStatusHeight = 0;
    if (myStatusBar != nullptr)
    {
      SendMessageW(myStatusBar, WM_SIZE, 0, 0);
      RECT aRect = {};
      GetWindowRect(myStatusBar, &aRect);
      aStatusHeight = aRect.bottom - aRect.top;
    }
    if (myViewWindow != nullptr)
    {
      MoveWindow(myViewWindow,
                 0,
                 aToolbarHeight,
                 static_cast<int>(aClient.right),
                 std::max<int>(1, aClient.bottom - aToolbarHeight - aStatusHeight),
                 TRUE);
    }
  }

  void OpenFileDialog()
  {
    std::vector<wchar_t> aFileName(32768, L'\0');
    OPENFILENAMEW aDialog = {};
    aDialog.lStructSize = sizeof(aDialog);
    aDialog.hwndOwner = myMainWindow;
    aDialog.lpstrFilter = L"STEP files (*.step;*.stp)\0*.step;*.stp\0All files (*.*)\0*.*\0\0";
    aDialog.lpstrFile = aFileName.data();
    aDialog.nMaxFile = static_cast<DWORD>(aFileName.size());
    aDialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    aDialog.lpstrDefExt = L"step";
    if (GetOpenFileNameW(&aDialog))
    {
      ImportStep(aFileName.data());
    }
  }

  void FitAll()
  {
    if (!myView.IsNull())
    {
      myView->FitAll(0.01, false);
      myView->ZFitAll();
      myView->Redraw();
    }
  }

  void SetIsometric()
  {
    if (!myView.IsNull())
    {
      myView->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
      FitAll();
    }
  }

  void ClearView()
  {
    if (!myContext.IsNull())
    {
      myContext->RemoveAll(true);
    }
    myCurrentPath.clear();
    SetWindowTextW(myMainWindow, L"CCEDA STEP Viewer");
    SetStatus(L"Ready");
  }

  void SetStatus(const std::wstring& theText) const
  {
    if (myStatusBar != nullptr)
    {
      SendMessageW(myStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(theText.c_str()));
    }
  }

  void ShowError(const std::wstring& theMessage)
  {
    SetStatus(theMessage);
    MessageBoxW(myMainWindow, theMessage.c_str(), L"CCEDA", MB_OK | MB_ICONERROR);
  }

  void HandleDrop(HDROP theDrop)
  {
    wchar_t aPath[32768] = {};
    if (DragQueryFileW(theDrop, 0, aPath, ARRAYSIZE(aPath)) > 0)
    {
      ImportStep(aPath);
    }
    DragFinish(theDrop);
  }

  void OnViewportResize()
  {
    if (!myView.IsNull())
    {
      myView->MustBeResized();
      myView->Redraw();
    }
  }

  void OnViewportPaint()
  {
    if (!myView.IsNull())
    {
      myView->Redraw();
    }
  }

  void OnViewportMouseDown(UINT theMessage, int theX, int theY)
  {
    SetFocus(myViewWindow);
    if (theMessage == WM_LBUTTONDOWN)
    {
      if (!myContext.IsNull())
      {
        myContext->MoveTo(theX, theY, myView, false);
        myContext->SelectDetected(AIS_SelectionScheme_Replace);
        myView->Redraw();
      }
      return;
    }

    myLastMouse = {theX, theY};
    myMouseMode = theMessage == WM_MBUTTONDOWN ? MouseMode::Pan : MouseMode::Rotate;
    SetCapture(myViewWindow);
    if (myMouseMode == MouseMode::Rotate && !myView.IsNull())
    {
      myView->StartRotation(theX, theY);
    }
  }

  void OnViewportMouseMove(int theX, int theY)
  {
    if (myView.IsNull())
    {
      return;
    }
    if (myMouseMode == MouseMode::Pan)
    {
      myView->Pan(theX - myLastMouse.x, myLastMouse.y - theY);
      myLastMouse = {theX, theY};
    }
    else if (myMouseMode == MouseMode::Rotate)
    {
      myView->Rotation(theX, theY);
    }
    else if (!myContext.IsNull())
    {
      myContext->MoveTo(theX, theY, myView, true);
    }
  }

  void OnViewportMouseUp()
  {
    if (myMouseMode != MouseMode::None)
    {
      myMouseMode = MouseMode::None;
      if (GetCapture() == myViewWindow)
      {
        ReleaseCapture();
      }
    }
  }

  void OnViewportMouseWheel(short theDelta)
  {
    if (!myView.IsNull())
    {
      const double aSteps = static_cast<double>(theDelta) / WHEEL_DELTA;
      myView->SetZoom(std::pow(1.15, aSteps), true);
      myView->Redraw();
    }
  }

  void HandleCommand(UINT theCommand)
  {
    switch (theCommand)
    {
      case CommandOpen:
        OpenFileDialog();
        break;
      case CommandFit:
        FitAll();
        break;
      case CommandIsometric:
        SetIsometric();
        break;
      case CommandClear:
        ClearView();
        break;
      case CommandExit:
        DestroyWindow(myMainWindow);
        break;
      case CommandAbout:
        MessageBoxW(myMainWindow,
                    L"CCEDA STEP Viewer\nOpen CASCADE Technology 8.0.0\n\n"
                    L"Left click: select\nMiddle drag: pan\nRight drag: rotate\nMouse wheel: zoom",
                    L"About CCEDA",
                    MB_OK | MB_ICONINFORMATION);
        break;
      default:
        break;
    }
  }

  static StepViewerApp* AppFromWindow(HWND theWindow)
  {
    return reinterpret_cast<StepViewerApp*>(GetWindowLongPtrW(theWindow, GWLP_USERDATA));
  }

  static LRESULT CALLBACK MainWindowProc(
    HWND theWindow, UINT theMessage, WPARAM theWParam, LPARAM theLParam)
  {
    if (theMessage == WM_NCCREATE)
    {
      const auto* aCreate = reinterpret_cast<CREATESTRUCTW*>(theLParam);
      SetWindowLongPtrW(
        theWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(aCreate->lpCreateParams));
      auto* anApp = reinterpret_cast<StepViewerApp*>(aCreate->lpCreateParams);
      if (anApp != nullptr)
      {
        anApp->myMainWindow = theWindow;
      }
    }

    StepViewerApp* anApp = AppFromWindow(theWindow);
    switch (theMessage)
    {
      case WM_CREATE:
        return anApp != nullptr && anApp->OnCreate() ? 0 : -1;
      case WM_SIZE:
        if (anApp != nullptr)
        {
          anApp->LayoutChildren();
        }
        return 0;
      case WM_COMMAND:
        if (anApp != nullptr)
        {
          anApp->HandleCommand(LOWORD(theWParam));
        }
        return 0;
      case WM_CLOSE:
        DestroyWindow(theWindow);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(theWindow, theMessage, theWParam, theLParam);
    }
  }

  static LRESULT CALLBACK ViewWindowProc(
    HWND theWindow, UINT theMessage, WPARAM theWParam, LPARAM theLParam)
  {
    if (theMessage == WM_NCCREATE)
    {
      const auto* aCreate = reinterpret_cast<CREATESTRUCTW*>(theLParam);
      SetWindowLongPtrW(
        theWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(aCreate->lpCreateParams));
    }

    StepViewerApp* anApp = AppFromWindow(theWindow);
    switch (theMessage)
    {
      case WM_ERASEBKGND:
        return 1;
      case WM_SIZE:
        if (anApp != nullptr)
        {
          anApp->OnViewportResize();
        }
        return 0;
      case WM_PAINT:
      {
        PAINTSTRUCT aPaint = {};
        BeginPaint(theWindow, &aPaint);
        if (anApp != nullptr)
        {
          anApp->OnViewportPaint();
        }
        EndPaint(theWindow, &aPaint);
        return 0;
      }
      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_RBUTTONDOWN:
        if (anApp != nullptr)
        {
          anApp->OnViewportMouseDown(
            theMessage, GET_X_LPARAM(theLParam), GET_Y_LPARAM(theLParam));
        }
        return 0;
      case WM_MBUTTONUP:
      case WM_RBUTTONUP:
      case WM_CAPTURECHANGED:
        if (anApp != nullptr)
        {
          anApp->OnViewportMouseUp();
        }
        return 0;
      case WM_MOUSEMOVE:
        if (anApp != nullptr)
        {
          anApp->OnViewportMouseMove(GET_X_LPARAM(theLParam), GET_Y_LPARAM(theLParam));
        }
        return 0;
      case WM_MOUSEWHEEL:
        if (anApp != nullptr)
        {
          anApp->OnViewportMouseWheel(GET_WHEEL_DELTA_WPARAM(theWParam));
        }
        return 0;
      case WM_DROPFILES:
        if (anApp != nullptr)
        {
          anApp->HandleDrop(reinterpret_cast<HDROP>(theWParam));
        }
        return 0;
      case WM_CONTEXTMENU:
        return 0;
      default:
        return DefWindowProcW(theWindow, theMessage, theWParam, theLParam);
    }
  }

private:
  enum class MouseMode
  {
    None,
    Pan,
    Rotate,
  };

  HINSTANCE myInstance = nullptr;
  HWND myMainWindow = nullptr;
  HWND myToolbar = nullptr;
  HWND myStatusBar = nullptr;
  HWND myViewWindow = nullptr;
  POINT myLastMouse = {};
  MouseMode myMouseMode = MouseMode::None;
  std::wstring myCurrentPath;

  occ::handle<Aspect_DisplayConnection> myDisplayConnection;
  occ::handle<OpenGl_GraphicDriver> myGraphicDriver;
  occ::handle<V3d_Viewer> myViewer;
  occ::handle<V3d_View> myView;
  occ::handle<WNT_Window> myOcctWindow;
  occ::handle<AIS_InteractiveContext> myContext;
};

int WINAPI wWinMain(HINSTANCE theInstance, HINSTANCE, PWSTR, int theShowCommand)
{
  SetProcessDPIAware();
  INITCOMMONCONTROLSEX aControls = {};
  aControls.dwSize = sizeof(aControls);
  aControls.dwICC = ICC_BAR_CLASSES;
  InitCommonControlsEx(&aControls);
  ConfigureOcctResources();

  StepViewerApp anApplication(theInstance);
  if (!anApplication.Create(theShowCommand))
  {
    MessageBoxW(nullptr,
                L"CCEDA could not create its main window.",
                L"CCEDA",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  int anArgumentCount = 0;
  LPWSTR* anArguments = CommandLineToArgvW(GetCommandLineW(), &anArgumentCount);
  if (anArguments != nullptr)
  {
    if (anArgumentCount > 1)
    {
      anApplication.ImportStep(anArguments[1]);
    }
    LocalFree(anArguments);
  }
  return anApplication.Run();
}
