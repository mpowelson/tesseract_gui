/*
 * Copyright (C) 2017 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <tesseract_gui/rendering/minimal_scene.h>
#include <tesseract_gui/common/gui_utils.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <QWidget>

#include <ignition/common/Console.hh>
#include <ignition/common/KeyEvent.hh>
#include <ignition/common/MouseEvent.hh>
#include <ignition/math/Vector2.hh>
#include <ignition/math/Vector3.hh>

// TODO(louise) Remove these pragmas once ign-rendering
// is disabling the warnings
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#include <ignition/rendering/Camera.hh>
#include <ignition/rendering/RayQuery.hh>
#include <ignition/rendering/RenderEngine.hh>
#include <ignition/rendering/RenderingIface.hh>
#include <ignition/rendering/Scene.hh>
#include <ignition/rendering/Grid.hh>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <ignition/gui/Application.hh>
#include <ignition/gui/Conversions.hh>
#include <ignition/gui/GuiEvents.hh>
#include <ignition/gui/Helpers.hh>
#include <ignition/gui/MainWindow.hh>

Q_DECLARE_METATYPE(tesseract_gui::RenderSync*)

/// \brief Private data class for IgnRenderer
class tesseract_gui::IgnRenderer::Implementation
{
  /// \brief Flag to indicate if mouse event is dirty
  public: bool mouseDirty{false};

  /// \brief Flag to indicate if hover event is dirty
  public: bool hoverDirty{false};

  /// \brief Flag to indicate if drop event is dirty
  public: bool dropDirty{false};

  /// \brief Mouse event
  public: ignition::common::MouseEvent mouseEvent;

  /// \brief Key event
  public: ignition::common::KeyEvent keyEvent;

  /// \brief Mutex to protect mouse events
  public: std::mutex mutex;

  /// \brief User camera
  public: ignition::rendering::CameraPtr camera{nullptr};

  /// \brief The currently hovered mouse position in screen coordinates
  public: ignition::math::Vector2i mouseHoverPos{ignition::math::Vector2i::Zero};

  /// \brief The currently drop mouse position in screen coordinates
  public: ignition::math::Vector2i mouseDropPos{ignition::math::Vector2i::Zero};

  /// \brief The dropped text in the scene
  public: std::string dropText{""};

  /// \brief Ray query for mouse clicks
  public: ignition::rendering::RayQueryPtr rayQuery{nullptr};

  /// \brief View control focus target
  public: ignition::math::Vector3d target;
};

/// \brief Qt and Ogre rendering is happening in different threads
/// The original sample 'textureinthread' from Qt used a double-buffer
/// scheme so that the worker (Ogre) thread write to FBO A, while
/// Qt is displaying FBO B.
///
/// However Qt's implementation doesn't handle all the edge cases
/// (like resizing a window), and also it increases our VRAM
/// consumption in multiple ways (since we have to double other
/// resources as well or re-architect certain parts of the code
/// to avoid it)
///
/// Thus we just serialize both threads so that when Qt reaches
/// drawing preparation, it halts and Ogre worker thread starts rendering,
/// then resumes when Ogre is done.
///
/// This code is admitedly more complicated than it should be
/// because Qt's synchronization using signals and slots causes
/// deadlocks when other means of synchronization are introduced.
/// The whole threaded loop should be rewritten.
///
/// All RenderSync does is conceptually:
///
/// \code
///   TextureNode::PrepareNode()
///   {
///     renderSync.WaitForWorkerThread(); // Qt thread
///       // WaitForQtThreadAndBlock();
///       // Now worker thread begins executing what's between
///       // ReleaseQtThreadFromBlock();
///     continue with qt code...
///   }
/// \endcode
///
///
/// For more info see
/// https://github.com/ignitionrobotics/ign-rendering/issues/304
class tesseract_gui::RenderSync
{
  /// \brief Cond. variable to synchronize rendering on specific events
  /// (e.g. texture resize) or for debugging (e.g. keep
  /// all API calls sequential)
  public: std::mutex mutex;

  /// \brief Cond. variable to synchronize rendering on specific events
  /// (e.g. texture resize) or for debugging (e.g. keep
  /// all API calls sequential)
  public: std::condition_variable cv;

  public: enum class RenderStallState
          {
            /// Qt is stuck inside WaitForWorkerThread
            /// Worker thread can proceed
            WorkerCanProceed,
            /// Qt is stuck inside WaitForWorkerThread
            /// Worker thread is between WaitForQtThreadAndBlock
            /// and ReleaseQtThreadFromBlock
            WorkerIsProceeding,
            /// Worker is stuck inside WaitForQtThreadAndBlock
            /// Qt can proceed
            QtCanProceed,
            /// Do not block
            ShuttingDown,
          };

  /// \brief See TextureNode::RenderSync::RenderStallState
  public: RenderStallState renderStallState =
      RenderStallState::QtCanProceed /*GUARDED_BY(sharedRenderMutex)*/;

  /// \brief Must be called from worker thread when we want to block
  /// \param[in] lock Acquired lock. Must be based on this->mutex
  public: void WaitForQtThreadAndBlock(std::unique_lock<std::mutex> &_lock);

  /// \brief Must be called from worker thread when we are done
  /// \param[in] lock Acquired lock. Must be based on this->mutex
  public: void ReleaseQtThreadFromBlock(std::unique_lock<std::mutex> &_lock);

  /// \brief Must be called from Qt thread periodically
  public: void WaitForWorkerThread();

  /// \brief Must be called from GUI thread when shutting down
  public: void Shutdown();
};

/// \brief Private data class for RenderWindowItem
class tesseract_gui::RenderWindowItem::Implementation
{
  /// \brief Keep latest mouse event
  public: ignition::common::MouseEvent mouseEvent;

  /// \brief Render thread
  public: RenderThread *renderThread = nullptr;

  /// \brief See RenderSync
  public: RenderSync renderSync;

  /// \brief List of threads
  public: static QList<QThread *> threads;

  /// \brief List of our QT connections.
  public: QList<QMetaObject::Connection> connections;
};

/// \brief Private data class for MinimalScene
class tesseract_gui::MinimalScene::Implementation
{
};

using namespace tesseract_gui;

QList<QThread *> RenderWindowItem::Implementation::threads;

/////////////////////////////////////////////////
void RenderSync::WaitForQtThreadAndBlock(std::unique_lock<std::mutex> &_lock)
{
  this->cv.wait(_lock, [this]
  { return this->renderStallState == RenderStallState::WorkerCanProceed ||
           this->renderStallState == RenderStallState::ShuttingDown; });

  this->renderStallState = RenderStallState::WorkerIsProceeding;
}

/////////////////////////////////////////////////
void RenderSync::ReleaseQtThreadFromBlock(std::unique_lock<std::mutex> &_lock)
{
  this->renderStallState = RenderStallState::QtCanProceed;
  _lock.unlock();
  this->cv.notify_one();
}

/////////////////////////////////////////////////
void RenderSync::WaitForWorkerThread()
{
  std::unique_lock<std::mutex> lock(this->mutex);

  // Wait until we're clear to go
  this->cv.wait( lock, [this]
  {
    return this->renderStallState == RenderStallState::QtCanProceed ||
           this->renderStallState == RenderStallState::ShuttingDown;
  } );

  // Worker thread asked us to wait!
  this->renderStallState = RenderStallState::WorkerCanProceed;

  lock.unlock();
  // Wake up worker thread
  this->cv.notify_one();
  lock.lock();

  // Wait until we're clear to go
  this->cv.wait( lock, [this]
  {
    return this->renderStallState == RenderStallState::QtCanProceed ||
           this->renderStallState == RenderStallState::ShuttingDown;
  } );
}

/////////////////////////////////////////////////
void RenderSync::Shutdown()
{
  {
    std::unique_lock<std::mutex> lock(this->mutex);

    this->renderStallState = RenderStallState::ShuttingDown;

    lock.unlock();
    this->cv.notify_one();
  }
}

/////////////////////////////////////////////////
IgnRenderer::IgnRenderer()
  : dataPtr(ignition::utils::MakeUniqueImpl<Implementation>())
{
}

/////////////////////////////////////////////////
void IgnRenderer::Render(RenderSync *_renderSync)
{
  std::unique_lock<std::mutex> lock(_renderSync->mutex);
  _renderSync->WaitForQtThreadAndBlock(lock);

  if (this->textureDirty)
  {
    // TODO(anyone) If SwapFromThread gets implemented,
    // then we only need to lock when texture is dirty
    // (but we still need to lock the whole routine if
    // debugging from RenderDoc or if user is not willing
    // to sacrifice VRAM)
    //
    // std::unique_lock<std::mutex> lock(renderSync->mutex);
    // _renderSync->WaitForQtThreadAndBlock(lock);
    this->dataPtr->camera->SetImageWidth(this->textureSize.width());
    this->dataPtr->camera->SetImageHeight(this->textureSize.height());
    this->dataPtr->camera->SetAspectRatio(this->textureSize.width() /
        this->textureSize.height());
    // setting the size should cause the render texture to be rebuilt
    this->dataPtr->camera->PreRender();
    this->textureDirty = false;

    // TODO(anyone) See SwapFromThread comments
    // _renderSync->ReleaseQtThreadFromBlock(lock);
  }

  this->textureId = this->dataPtr->camera->RenderTextureGLId();

  // view control
  this->HandleMouseEvent();

  if (tesseract_gui::getApp())
  {
    tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), new ignition::gui::events::PreRender());

//    ignition::gui::App()->sendEvent(
//        ignition::gui::App()->findChild<ignition::gui::MainWindow *>(),
//        new ignition::gui::events::PreRender());
  }

  // update and render to texture
  this->dataPtr->camera->Update();

  if (tesseract_gui::getApp())
  {
    tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), new ignition::gui::events::Render());
//    ignition::gui::App()->sendEvent(
//        ignition::gui::App()->findChild<ignition::gui::MainWindow *>(),
//        new ignition::gui::events::Render());
  }
  _renderSync->ReleaseQtThreadFromBlock(lock);
}

/////////////////////////////////////////////////
void IgnRenderer::HandleMouseEvent()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  this->BroadcastHoverPos();
  this->BroadcastDrag();
  this->BroadcastMousePress();
  this->BroadcastLeftClick();
  this->BroadcastRightClick();
  this->BroadcastScroll();
  this->BroadcastKeyPress();
  this->BroadcastKeyRelease();
  this->BroadcastDrop();
  this->dataPtr->mouseDirty = false;
}

////////////////////////////////////////////////
void IgnRenderer::HandleKeyPress(const ignition::common::KeyEvent &_e)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

  this->dataPtr->keyEvent = _e;

  this->dataPtr->mouseEvent.SetControl(this->dataPtr->keyEvent.Control());
  this->dataPtr->mouseEvent.SetShift(this->dataPtr->keyEvent.Shift());
  this->dataPtr->mouseEvent.SetAlt(this->dataPtr->keyEvent.Alt());
}

////////////////////////////////////////////////
void IgnRenderer::HandleKeyRelease(const ignition::common::KeyEvent &_e)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

  this->dataPtr->keyEvent = _e;

  this->dataPtr->mouseEvent.SetControl(this->dataPtr->keyEvent.Control());
  this->dataPtr->mouseEvent.SetShift(this->dataPtr->keyEvent.Shift());
  this->dataPtr->mouseEvent.SetAlt(this->dataPtr->keyEvent.Alt());
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastDrop()
{
  if (!this->dataPtr->dropDirty)
    return;
  ignition::gui::events::DropOnScene dropOnSceneEvent(
    this->dataPtr->dropText, this->dataPtr->mouseDropPos);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &dropOnSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &dropOnSceneEvent);
  this->dataPtr->dropDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastHoverPos()
{
  if (!this->dataPtr->hoverDirty)
    return;

  auto pos = this->ScreenToScene(this->dataPtr->mouseHoverPos);

  ignition::gui::events::HoverToScene hoverToSceneEvent(pos);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &hoverToSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &hoverToSceneEvent);

  ignition::common::MouseEvent hoverMouseEvent = this->dataPtr->mouseEvent;
  hoverMouseEvent.SetPos(this->dataPtr->mouseHoverPos);
  hoverMouseEvent.SetDragging(false);
  hoverMouseEvent.SetType(ignition::common::MouseEvent::MOVE);
  ignition::gui::events::HoverOnScene hoverOnSceneEvent(hoverMouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &hoverOnSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &hoverOnSceneEvent);

  this->dataPtr->hoverDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastDrag()
{
  if (!this->dataPtr->mouseDirty)
    return;

  // Only broadcast drag if dragging
  if (!this->dataPtr->mouseEvent.Dragging())
    return;

  ignition::gui::events::DragOnScene dragEvent(this->dataPtr->mouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &dragEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &dragEvent);

  this->dataPtr->mouseDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastLeftClick()
{
  if (!this->dataPtr->mouseDirty)
    return;

  if (this->dataPtr->mouseEvent.Button() != ignition::common::MouseEvent::LEFT ||
      this->dataPtr->mouseEvent.Type() != ignition::common::MouseEvent::RELEASE)
    return;

  auto pos = this->ScreenToScene(this->dataPtr->mouseEvent.Pos());

  ignition::gui::events::LeftClickToScene leftClickToSceneEvent(pos);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &leftClickToSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &leftClickToSceneEvent);

  ignition::gui::events::LeftClickOnScene leftClickOnSceneEvent(this->dataPtr->mouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &leftClickOnSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &leftClickOnSceneEvent);

  this->dataPtr->mouseDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastRightClick()
{
  if (!this->dataPtr->mouseDirty)
    return;

  if (this->dataPtr->mouseEvent.Button() != ignition::common::MouseEvent::RIGHT ||
      this->dataPtr->mouseEvent.Type() != ignition::common::MouseEvent::RELEASE)
    return;

  auto pos = this->ScreenToScene(this->dataPtr->mouseEvent.Pos());

  ignition::gui::events::RightClickToScene rightClickToSceneEvent(pos);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &rightClickToSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &rightClickToSceneEvent);

  ignition::gui::events::RightClickOnScene rightClickOnSceneEvent(this->dataPtr->mouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &rightClickOnSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &rightClickOnSceneEvent);

  this->dataPtr->mouseDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastMousePress()
{
  if (!this->dataPtr->mouseDirty)
    return;

  if (this->dataPtr->mouseEvent.Type() != ignition::common::MouseEvent::PRESS)
    return;

  ignition::gui::events::MousePressOnScene event(this->dataPtr->mouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &event);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &event);

  this->dataPtr->mouseDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastScroll()
{
  if (!this->dataPtr->mouseDirty)
    return;

  if (this->dataPtr->mouseEvent.Type() != ignition::common::MouseEvent::SCROLL)
    return;

  ignition::gui::events::ScrollOnScene scrollOnSceneEvent(this->dataPtr->mouseEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &scrollOnSceneEvent);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &scrollOnSceneEvent);

  this->dataPtr->mouseDirty = false;
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastKeyRelease()
{
  if (this->dataPtr->keyEvent.Type() != ignition::common::KeyEvent::RELEASE)
    return;

  ignition::gui::events::KeyReleaseOnScene keyRelease(this->dataPtr->keyEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &keyRelease);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &keyRelease);

  this->dataPtr->keyEvent.SetType(ignition::common::KeyEvent::NO_EVENT);
}

/////////////////////////////////////////////////
void IgnRenderer::BroadcastKeyPress()
{
  if (this->dataPtr->keyEvent.Type() != ignition::common::KeyEvent::PRESS)
    return;

  ignition::gui::events::KeyPressOnScene keyPress(this->dataPtr->keyEvent);
  tesseract_gui::getApp()->sendEvent(tesseract_gui::getApp(), &keyPress);
//  ignition::gui::App()->sendEvent(ignition::gui::App()->findChild<ignition::gui::MainWindow *>(), &keyPress);

  this->dataPtr->keyEvent.SetType(ignition::common::KeyEvent::NO_EVENT);
}

/////////////////////////////////////////////////
void IgnRenderer::Initialize()
{
  if (this->initialized)
    return;

  std::map<std::string, std::string> params;
  params["useCurrentGLContext"] = "1";
//  params["winID"] = std::to_string(tesseract_gui::getApp()->activeWindow()->winId());
//  params["winID"] = std::to_string(
//    ignition::gui::App()->findChild<ignition::gui::MainWindow *>()->
//      QuickWindow()-winId());
  auto engine = ignition::rendering::engine(this->engineName, params);
  if (!engine)
  {
    ignerr << "Engine [" << this->engineName << "] is not supported"
           << std::endl;
    return;
  }

  // Scene
  auto scene = engine->SceneByName(this->sceneName);
  if (!scene)
  {
    igndbg << "Create scene [" << this->sceneName << "]" << std::endl;
    scene = engine->CreateScene(this->sceneName);
    scene->SetAmbientLight(this->ambientLight);
    scene->SetBackgroundColor(this->backgroundColor);
  }

  if (this->skyEnable)
  {
    scene->SetSkyEnabled(true);
  }

  if (this->gridEnable)
  {
    ignition::rendering::VisualPtr visual = scene->VisualByName("tesseract_grid");
    if (visual == nullptr)
    {
      ignition::rendering::VisualPtr root = scene->RootVisual();

      // create gray material
      ignition::rendering::MaterialPtr gray = scene->CreateMaterial();
      gray->SetAmbient(0.7, 0.7, 0.7);
      gray->SetDiffuse(0.7, 0.7, 0.7);
      gray->SetSpecular(0.7, 0.7, 0.7);

      // create grid visual
      unsigned id = 1000; //static_cast<unsigned>(this->dataPtr->entity_manager.addVisual("tesseract_grid"));
      ignition::rendering::VisualPtr visual = scene->CreateVisual(id, "tesseract_grid");
      ignition::rendering::GridPtr gridGeom = scene->CreateGrid();
      if (!gridGeom)
      {
        ignwarn << "Failed to create grid for scene ["
          << scene->Name() << "] on engine ["
            << scene->Engine()->Name() << "]"
              << std::endl;
        return;
      }
      gridGeom->SetCellCount(20);
      gridGeom->SetCellLength(1);
      gridGeom->SetVerticalCellCount(0);
      visual->AddGeometry(gridGeom);
      visual->SetLocalPosition(0, 0, 0.015);
      visual->SetMaterial(gray);
      root->AddChild(visual);
    }
    else
    {
      visual->SetVisible(true);
    }
  }

  auto root = scene->RootVisual();

  // Camera
  this->dataPtr->camera = scene->CreateCamera();
  this->dataPtr->camera->SetUserData("user-camera", true);
  root->AddChild(this->dataPtr->camera);
  this->dataPtr->camera->SetLocalPose(this->cameraPose);
  this->dataPtr->camera->SetNearClipPlane(this->cameraNearClip);
  this->dataPtr->camera->SetFarClipPlane(this->cameraFarClip);
  this->dataPtr->camera->SetImageWidth(this->textureSize.width());
  this->dataPtr->camera->SetImageHeight(this->textureSize.height());
  this->dataPtr->camera->SetAntiAliasing(8);
  this->dataPtr->camera->SetHFOV(M_PI * 0.5);
  // setting the size and calling PreRender should cause the render texture to
  // be rebuilt
  this->dataPtr->camera->PreRender();
  this->textureId = this->dataPtr->camera->RenderTextureGLId();

  // Ray Query
  this->dataPtr->rayQuery = this->dataPtr->camera->Scene()->CreateRayQuery();

  this->initialized = true;
}

/////////////////////////////////////////////////
void IgnRenderer::Destroy()
{
  auto engine = ignition::rendering::engine(this->engineName);
  if (!engine)
    return;
  auto scene = engine->SceneByName(this->sceneName);
  if (!scene)
    return;
  scene->DestroySensor(this->dataPtr->camera);

  // If that was the last sensor, destroy scene
  if (scene->SensorCount() == 0)
  {
    igndbg << "Destroy scene [" << scene->Name() << "]" << std::endl;
    engine->DestroyScene(scene);

    // TODO(anyone) If that was the last scene, terminate engine?
  }
}

/////////////////////////////////////////////////
void IgnRenderer::NewHoverEvent(const ignition::math::Vector2i &_hoverPos)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  this->dataPtr->mouseHoverPos = _hoverPos;
  this->dataPtr->hoverDirty = true;
}

/////////////////////////////////////////////////
void IgnRenderer::NewDropEvent(const std::string &_dropText,
  const ignition::math::Vector2i &_dropPos)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  this->dataPtr->dropText = _dropText;
  this->dataPtr->mouseDropPos = _dropPos;
  this->dataPtr->dropDirty = true;
}

/////////////////////////////////////////////////
void IgnRenderer::NewMouseEvent(const ignition::common::MouseEvent &_e)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  this->dataPtr->mouseEvent = _e;
  this->dataPtr->mouseDirty = true;
}

/////////////////////////////////////////////////
ignition::math::Vector3d IgnRenderer::ScreenToScene(
    const ignition::math::Vector2i &_screenPos) const
{
  // TODO(ahcorde): Replace this code with function in ign-rendering
  // Require this commit
  // https://github.com/ignitionrobotics/ign-rendering/pull/363
  // in ign-rendering6

  // Normalize point on the image
  double width = this->dataPtr->camera->ImageWidth();
  double height = this->dataPtr->camera->ImageHeight();

  double nx = 2.0 * _screenPos.X() / width - 1.0;
  double ny = 1.0 - 2.0 * _screenPos.Y() / height;

  // Make a ray query
  this->dataPtr->rayQuery->SetFromCamera(
      this->dataPtr->camera, ignition::math::Vector2d(nx, ny));

  auto result = this->dataPtr->rayQuery->ClosestPoint();
  if (result)
    return result.point;

  // Set point to be 10m away if no intersection found
  return this->dataPtr->rayQuery->Origin() +
      this->dataPtr->rayQuery->Direction() * 10;
}

/////////////////////////////////////////////////
RenderThread::RenderThread()
{
  RenderWindowItem::Implementation::threads << this;
  qRegisterMetaType<RenderSync*>("RenderSync*");
}

/////////////////////////////////////////////////
void RenderThread::RenderNext(RenderSync *_renderSync)
{
  this->context->makeCurrent(this->surface);

  if (!this->ignRenderer.initialized)
  {
    // Initialize renderer
    this->ignRenderer.Initialize();
  }

  // check if engine has been successfully initialized
  if (!this->ignRenderer.initialized)
  {
    ignerr << "Unable to initialize renderer" << std::endl;
    return;
  }

  this->ignRenderer.Render(_renderSync);

  emit TextureReady(this->ignRenderer.textureId, this->ignRenderer.textureSize);
}

/////////////////////////////////////////////////
void RenderThread::ShutDown()
{
  this->context->makeCurrent(this->surface);

  this->ignRenderer.Destroy();

  this->context->doneCurrent();
  delete this->context;

  // schedule this to be deleted only after we're done cleaning up
  this->surface->deleteLater();

  // Stop event processing, move the thread to GUI and make sure it is deleted.
  this->exit();
  this->moveToThread(QGuiApplication::instance()->thread());
}

/////////////////////////////////////////////////
void RenderThread::SizeChanged()
{
  auto item = qobject_cast<QQuickItem *>(this->sender());
  if (!item)
  {
    ignerr << "Internal error, sender is not QQuickItem." << std::endl;
    return;
  }

  if (item->width() <= 0 || item->height() <= 0)
    return;

  this->ignRenderer.textureSize = QSize(item->width(), item->height());
  this->ignRenderer.textureDirty = true;
}

/////////////////////////////////////////////////
TextureNode::TextureNode(QQuickWindow *_window, RenderSync &_renderSync)
    : renderSync(_renderSync), window(_window)
{
  // Our texture node must have a texture, so use the default 0 texture.
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
  this->texture = this->window->createTextureFromId(0, QSize(1, 1));
#else
  void * nativeLayout;
  this->texture = this->window->createTextureFromNativeObject(
      QQuickWindow::NativeObjectTexture, &nativeLayout, 0, QSize(1, 1),
      QQuickWindow::TextureIsOpaque);
#endif
  this->setTexture(this->texture);
}

/////////////////////////////////////////////////
TextureNode::~TextureNode()
{
  delete this->texture;
}

/////////////////////////////////////////////////
void TextureNode::NewTexture(uint _id, const QSize &_size)
{
  this->mutex.lock();
  this->id = _id;
  this->size = _size;
  this->mutex.unlock();

  // We cannot call QQuickWindow::update directly here, as this is only allowed
  // from the rendering thread or GUI thread.
  emit PendingNewTexture();
}

/////////////////////////////////////////////////
void TextureNode::PrepareNode()
{
  this->mutex.lock();
  uint newId = this->id;
  QSize sz = this->size;
  this->id = 0;
  this->mutex.unlock();
  if (newId)
  {
    delete this->texture;
    // note: include QQuickWindow::TextureHasAlphaChannel if the rendered
    // content has alpha.
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
    this->texture = this->window->createTextureFromId(
        newId, sz, QQuickWindow::TextureIsOpaque);
#else
    // TODO(anyone) Use createTextureFromNativeObject
    // https://github.com/ignitionrobotics/ign-gui/issues/113
#ifndef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    this->texture = this->window->createTextureFromId(
        newId, sz, QQuickWindow::TextureIsOpaque);
#ifndef _WIN32
# pragma GCC diagnostic pop
#endif

#endif
    this->setTexture(this->texture);

    this->markDirty(DirtyMaterial);

    // This will notify the rendering thread that the texture is now being
    // rendered and it can start rendering to the other one.
    // emit TextureInUse(&this->renderSync); See comment below
  }
  // NOTE: The original code from Qt samples only emitted when
  // newId is not null.
  //
  // This is correct... for their case.
  // However we need to synchronize the threads when resolution changes,
  // and we're also currently doing everything in lockstep (i.e. both Qt
  // and worker thread are serialized,
  // see https://github.com/ignitionrobotics/ign-rendering/issues/304 )
  //
  // We need to emit even if newId == 0 because it's safe as long as both
  // threads are forcefully serialized and otherwise we may get a
  // deadlock (this func. called twice in a row with the worker thread still
  // finishing the 1st iteration, may result in a deadlock for newer versions
  // of Qt; as WaitForWorkerThread will be called with no corresponding
  // WaitForQtThreadAndBlock as the worker thread thinks there are
  // no more jobs to do.
  //
  // If we want these to run in worker thread and stay resolution-synchronized,
  // we probably should use a different method of signals and slots
  // to send work to the worker thread and get results back
  emit TextureInUse(&this->renderSync);

  this->renderSync.WaitForWorkerThread();
}

/////////////////////////////////////////////////
RenderWindowItem::RenderWindowItem(QQuickItem *_parent)
  : QQuickItem(_parent), dataPtr(ignition::utils::MakeUniqueImpl<Implementation>())
{
  this->setAcceptedMouseButtons(Qt::AllButtons);
  this->setFlag(ItemHasContents);
  this->dataPtr->renderThread = new RenderThread();
}

/////////////////////////////////////////////////
RenderWindowItem::~RenderWindowItem()
{
  // Disconnect our QT connections.
  for(auto conn : this->dataPtr->connections)
    QObject::disconnect(conn);

  this->dataPtr->renderSync.Shutdown();
  QMetaObject::invokeMethod(this->dataPtr->renderThread,
                            "ShutDown",
                            Qt::QueuedConnection);

  this->dataPtr->renderThread->wait();
}

/////////////////////////////////////////////////
void RenderWindowItem::Ready()
{
  this->dataPtr->renderThread->surface = new QOffscreenSurface();
  this->dataPtr->renderThread->surface->setFormat(
      this->dataPtr->renderThread->context->format());
  this->dataPtr->renderThread->surface->create();

  this->dataPtr->renderThread->ignRenderer.textureSize =
      QSize(std::max({this->width(), 1.0}), std::max({this->height(), 1.0}));

  this->dataPtr->renderThread->moveToThread(this->dataPtr->renderThread);

  this->connect(this, &QQuickItem::widthChanged,
      this->dataPtr->renderThread, &RenderThread::SizeChanged);
  this->connect(this, &QQuickItem::heightChanged,
      this->dataPtr->renderThread, &RenderThread::SizeChanged);

  this->dataPtr->renderThread->start();
  this->update();
}

/////////////////////////////////////////////////
QSGNode *RenderWindowItem::updatePaintNode(QSGNode *_node,
    QQuickItem::UpdatePaintNodeData * /*_data*/)
{
  TextureNode *node = static_cast<TextureNode *>(_node);

  if (!this->dataPtr->renderThread->context)
  {
    QOpenGLContext *current = this->window()->openglContext();
    // Some GL implementations require that the currently bound context is
    // made non-current before we set up sharing, so we doneCurrent here
    // and makeCurrent down below while setting up our own context.
    current->doneCurrent();

    this->dataPtr->renderThread->context = new QOpenGLContext();
    this->dataPtr->renderThread->context->setFormat(current->format());
    this->dataPtr->renderThread->context->setShareContext(current);
    this->dataPtr->renderThread->context->create();
    this->dataPtr->renderThread->context->moveToThread(
        this->dataPtr->renderThread);

    current->makeCurrent(this->window());

    QMetaObject::invokeMethod(this, "Ready");
    return nullptr;
  }

  if (!node)
  {
    node = new TextureNode(this->window(), this->dataPtr->renderSync);

    // Set up connections to get the production of render texture in sync with
    // vsync on the rendering thread.
    //
    // When a new texture is ready on the rendering thread, we use a direct
    // connection to the texture node to let it know a new texture can be used.
    // The node will then emit PendingNewTexture which we bind to
    // QQuickWindow::update to schedule a redraw.
    //
    // When the scene graph starts rendering the next frame, the PrepareNode()
    // function is used to update the node with the new texture. Once it
    // completes, it emits TextureInUse() which we connect to the rendering
    // thread's RenderNext() to have it start producing content into its render
    // texture.
    //
    // This rendering pipeline is throttled by vsync on the scene graph
    // rendering thread.

    this->dataPtr->connections << this->connect(this->dataPtr->renderThread,
        &RenderThread::TextureReady, node, &TextureNode::NewTexture,
        Qt::DirectConnection);
    this->dataPtr->connections << this->connect(node,
        &TextureNode::PendingNewTexture, this->window(),
        &QQuickWindow::update, Qt::QueuedConnection);
    this->dataPtr->connections << this->connect(this->window(),
        &QQuickWindow::beforeRendering, node, &TextureNode::PrepareNode,
        Qt::DirectConnection);
    this->dataPtr->connections << this->connect(node,
        &TextureNode::TextureInUse, this->dataPtr->renderThread,
        &RenderThread::RenderNext, Qt::QueuedConnection);

    // Get the production of FBO textures started..
    QMetaObject::invokeMethod(this->dataPtr->renderThread, "RenderNext",
      Qt::QueuedConnection,
      Q_ARG(RenderSync*, &node->renderSync));
  }

  node->setRect(this->boundingRect());

  return node;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetBackgroundColor(const ignition::math::Color &_color)
{
  this->dataPtr->renderThread->ignRenderer.backgroundColor = _color;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetAmbientLight(const ignition::math::Color &_ambient)
{
  this->dataPtr->renderThread->ignRenderer.ambientLight = _ambient;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetEngineName(const std::string &_name)
{
  this->dataPtr->renderThread->ignRenderer.engineName = _name;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetSceneName(const std::string &_name)
{
  this->dataPtr->renderThread->ignRenderer.sceneName = _name;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetCameraPose(const ignition::math::Pose3d &_pose)
{
  this->dataPtr->renderThread->ignRenderer.cameraPose = _pose;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetCameraNearClip(double _near)
{
  this->dataPtr->renderThread->ignRenderer.cameraNearClip = _near;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetCameraFarClip(double _far)
{
  this->dataPtr->renderThread->ignRenderer.cameraFarClip = _far;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetSceneService(const std::string &_service)
{
  this->dataPtr->renderThread->ignRenderer.sceneService = _service;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetPoseTopic(const std::string &_topic)
{
  this->dataPtr->renderThread->ignRenderer.poseTopic = _topic;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetDeletionTopic(const std::string &_topic)
{
  this->dataPtr->renderThread->ignRenderer.deletionTopic = _topic;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetSceneTopic(const std::string &_topic)
{
  this->dataPtr->renderThread->ignRenderer.sceneTopic = _topic;
}

/////////////////////////////////////////////////
void RenderWindowItem::SetSkyEnabled(const bool &_sky)
{
  this->dataPtr->renderThread->ignRenderer.skyEnable = _sky;
}

void RenderWindowItem::SetGridEnabled(bool _grid)
{
  this->dataPtr->renderThread->ignRenderer.gridEnable = _grid;
}

/////////////////////////////////////////////////
MinimalScene::MinimalScene()
  : QObject()
  , dataPtr(ignition::utils::MakeUniqueImpl<Implementation>())
{
  qmlRegisterType<RenderWindowItem>("RenderWindow", 1, 0, "RenderWindow");
  qDebug() << "This is a test";
}

/////////////////////////////////////////////////
//void MinimalScene::LoadConfig(const tinyxml2::XMLElement *_pluginElem)
//{
//  RenderWindowItem *renderWindow =
//      this->PluginItem()->findChild<RenderWindowItem *>();
//  if (!renderWindow)
//  {
//    ignerr << "Unable to find Render Window item. "
//           << "Render window will not be created" << std::endl;
//    return;
//  }

//  if (this->title.empty())
//    this->title = "3D Scene";

//  std::string cmdRenderEngine = ignition::gui::renderEngineName();
//  // Custom parameters
//  if (_pluginElem)
//  {
//    // Only pick engine from XML if none is set on the Window
//    if (cmdRenderEngine.empty())
//    {
//      auto elem = _pluginElem->FirstChildElement("engine");
//      if (nullptr != elem && nullptr != elem->GetText())
//      {
//        cmdRenderEngine = elem->GetText();
//      }
//    }

//    auto elem = _pluginElem->FirstChildElement("scene");
//    if (nullptr != elem && nullptr != elem->GetText())
//      renderWindow->SetSceneName(elem->GetText());

//    elem = _pluginElem->FirstChildElement("ambient_light");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      math::Color ambient;
//      std::stringstream colorStr;
//      colorStr << std::string(elem->GetText());
//      colorStr >> ambient;
//      renderWindow->SetAmbientLight(ambient);
//    }

//    elem = _pluginElem->FirstChildElement("background_color");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      math::Color bgColor;
//      std::stringstream colorStr;
//      colorStr << std::string(elem->GetText());
//      colorStr >> bgColor;
//      renderWindow->SetBackgroundColor(bgColor);
//    }

//    elem = _pluginElem->FirstChildElement("camera_pose");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      math::Pose3d pose;
//      std::stringstream poseStr;
//      poseStr << std::string(elem->GetText());
//      poseStr >> pose;
//      renderWindow->SetCameraPose(pose);
//    }

//    elem = _pluginElem->FirstChildElement("camera_clip");
//    if (nullptr != elem && !elem->NoChildren())
//    {
//      auto child = elem->FirstChildElement("near");
//      if (nullptr != child && nullptr != child->GetText())
//      {
//        double n;
//        std::stringstream nearStr;
//        nearStr << std::string(child->GetText());
//        nearStr >> n;
//        if (nearStr.fail())
//        {
//          ignerr << "Unable to set <near> to '" << nearStr.str()
//                 << "' using default near clip distance" << std::endl;
//        }
//        else
//        {
//          renderWindow->SetCameraNearClip(n);
//        }
//      }

//      child = elem->FirstChildElement("far");
//      if (nullptr != child && nullptr != child->GetText())
//      {
//        double f;
//        std::stringstream farStr;
//        farStr << std::string(child->GetText());
//        farStr >> f;
//        if (farStr.fail())
//        {
//          ignerr << "Unable to set <far> to '" << farStr.str()
//                 << "' using default far clip distance" << std::endl;
//        }
//        else
//        {
//          renderWindow->SetCameraFarClip(f);
//        }
//      }
//    }

//    elem = _pluginElem->FirstChildElement("service");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      std::string service = elem->GetText();
//      renderWindow->SetSceneService(service);
//    }

//    elem = _pluginElem->FirstChildElement("pose_topic");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      std::string topic = elem->GetText();
//      renderWindow->SetPoseTopic(topic);
//    }

//    elem = _pluginElem->FirstChildElement("deletion_topic");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      std::string topic = elem->GetText();
//      renderWindow->SetDeletionTopic(topic);
//    }

//    elem = _pluginElem->FirstChildElement("scene_topic");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      std::string topic = elem->GetText();
//      renderWindow->SetSceneTopic(topic);
//    }

//    elem = _pluginElem->FirstChildElement("sky");
//    if (nullptr != elem && nullptr != elem->GetText())
//    {
//      renderWindow->SetSkyEnabled(true);
//      if (!elem->NoChildren())
//        ignwarn << "Child elements of <sky> are not supported yet" << std::endl;
//    }
//  }

//  renderWindow->SetEngineName(cmdRenderEngine);
//  // there is a problem with displaying ogre2 render textures that are in
//  // sRGB format. Workaround for now is to apply gamma correction
//  // manually.
//  // There maybe a better way to solve the problem by making OpenGL calls.
//  if (cmdRenderEngine == std::string("ogre2"))
//    this->PluginItem()->setProperty("gammaCorrect", true);
//}

/////////////////////////////////////////////////
void RenderWindowItem::OnHovered(int _mouseX, int _mouseY)
{
  this->dataPtr->renderThread->ignRenderer.NewHoverEvent({_mouseX, _mouseY});
}

/////////////////////////////////////////////////
void RenderWindowItem::OnDropped(const QString &_drop, int _mouseX, int _mouseY)
{
  this->dataPtr->renderThread->ignRenderer.NewDropEvent(_drop.toStdString(), {_mouseX, _mouseY});
}

/////////////////////////////////////////////////
void RenderWindowItem::mousePressEvent(QMouseEvent *_e)
{
  this->dataPtr->mouseEvent = ignition::gui::convert(*_e);
  this->dataPtr->mouseEvent.SetPressPos(this->dataPtr->mouseEvent.Pos());

  this->dataPtr->renderThread->ignRenderer.NewMouseEvent(
      this->dataPtr->mouseEvent);
}

////////////////////////////////////////////////
void RenderWindowItem::keyPressEvent(QKeyEvent *_e)
{
  if (_e->isAutoRepeat())
    return;

  auto event = ignition::gui::convert(*_e);
  this->HandleKeyPress(event);
}

////////////////////////////////////////////////
void RenderWindowItem::keyReleaseEvent(QKeyEvent *_e)
{
  if (_e->isAutoRepeat())
    return;

  auto event = ignition::gui::convert(*_e);
  this->HandleKeyPress(event);
}

////////////////////////////////////////////////
void RenderWindowItem::mouseReleaseEvent(QMouseEvent *_e)
{
  // Store values that depend on previous events
  auto pressPos = this->dataPtr->mouseEvent.PressPos();
  auto dragging = this->dataPtr->mouseEvent.Dragging();

  this->dataPtr->mouseEvent = ignition::gui::convert(*_e);
  this->dataPtr->mouseEvent.SetPressPos(pressPos);
  this->dataPtr->mouseEvent.SetDragging(dragging);

  this->dataPtr->renderThread->ignRenderer.NewMouseEvent(
      this->dataPtr->mouseEvent);
}

////////////////////////////////////////////////
void RenderWindowItem::mouseMoveEvent(QMouseEvent *_e)
{
  // Store values that depend on previous events
  auto pressPos = this->dataPtr->mouseEvent.PressPos();

  this->dataPtr->mouseEvent = ignition::gui::convert(*_e);

  if (this->dataPtr->mouseEvent.Dragging())
    this->dataPtr->mouseEvent.SetPressPos(pressPos);

  this->dataPtr->renderThread->ignRenderer.NewMouseEvent(
      this->dataPtr->mouseEvent);
}

////////////////////////////////////////////////
void RenderWindowItem::wheelEvent(QWheelEvent *_e)
{
  this->forceActiveFocus();

  this->dataPtr->mouseEvent = ignition::gui::convert(*_e);
  this->dataPtr->renderThread->ignRenderer.NewMouseEvent(
    this->dataPtr->mouseEvent);
}

////////////////////////////////////////////////
void RenderWindowItem::HandleKeyPress(const ignition::common::KeyEvent &_e)
{
  this->dataPtr->renderThread->ignRenderer.HandleKeyPress(_e);
}

////////////////////////////////////////////////
void RenderWindowItem::HandleKeyRelease(const ignition::common::KeyEvent &_e)
{
  this->dataPtr->renderThread->ignRenderer.HandleKeyRelease(_e);
}

/////////////////////////////////////////////////
void MinimalScene::onHovered(int _mouseX, int _mouseY)
{
//  auto renderWindow = this->PluginItem()->findChild<RenderWindowItem *>();
  auto renderWindow = tesseract_gui::getApp()->findChild<RenderWindowItem *>();
  renderWindow->OnHovered(_mouseX, _mouseY);
}

/////////////////////////////////////////////////
void MinimalScene::onDropped(const QString &_drop, int _mouseX, int _mouseY)
{
//  auto renderWindow = this->PluginItem()->findChild<RenderWindowItem *>();
  auto renderWindow = tesseract_gui::getApp()->findChild<RenderWindowItem *>();
  renderWindow->OnDropped(_drop, _mouseX, _mouseY);
}

/////////////////////////////////////////////////
void MinimalScene::onFocusWindow()
{
//  auto renderWindow = this->PluginItem()->findChild<RenderWindowItem *>();
  auto renderWindow = tesseract_gui::getApp()->findChild<RenderWindowItem *>();
  renderWindow->forceActiveFocus();
}
