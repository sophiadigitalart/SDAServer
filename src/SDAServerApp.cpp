#include "cinder/app/App.h"
#include "cinder/app/RendererGl.h"
#include "cinder/gl/gl.h"
#include "cinder/Utilities.h"
#include "cinder/Log.h"
#include "cinder/Json.h"
#include <list>
#include "MidiIn.h"
#include "MidiMessage.h"
#include "MidiConstants.h"
#include "cinder/osc/Osc.h"
// WebSockets
#include "WebSocketClient.h"
#include "WebSocketServer.h"
// Settings
#include "SDASettings.h"
// Session
#include "SDASession.h"
// Log
#include "SDALog.h"
// Spout
#include "CiSpoutOut.h"

using namespace ci;
using namespace ci::app;
using namespace std;
using namespace SophiaDigitalArt;

using Receiver = osc::ReceiverUdp;
using protocol = asio::ip::udp;

const uint16_t localPort = 7000;

class SDAServerApp : public App {

public:
	SDAServerApp();
	void mouseMove(MouseEvent event) override;
	void mouseDown(MouseEvent event) override;
	void mouseDrag(MouseEvent event) override;
	void mouseUp(MouseEvent event) override;
	void keyDown(KeyEvent event) override;
	void keyUp(KeyEvent event) override;
	void fileDrop(FileDropEvent event) override;
	void update() override;
	void draw() override;
	void cleanup() override;
	void setUIVisibility(bool visible);
private:
	// Settings
	SDASettingsRef					mSDASettings;
	// Session
	SDASessionRef					mSDASession;
	// Log
	SDALogRef						mSDALog;
	// imgui
	float							color[4];
	float							backcolor[4];
	int								playheadPositions[12];
	int								speeds[12];

	float							f = 0.0f;
	char							buf[64];
	unsigned int					i, j;

	bool							mouseGlobal;

	string							mError;
	// fbo
	bool							mIsShutDown;
	Anim<float>						mRenderWindowTimer;
	void							positionRenderWindow();
	bool							mFadeInDelay;
	SpoutOut 						mSpoutOut;

	// OSC
	Receiver						mReceiver;
	std::map<uint64_t, protocol::endpoint> mConnections;
	ivec2							mCurrentCirclePos;
	vec2							mCurrentSquarePos;
	bool							mMouseDown = false;
};


SDAServerApp::SDAServerApp()
	: mSpoutOut("SDAServer", app::getWindowSize()),
	mReceiver(localPort)
{
	// Settings
	mSDASettings = SDASettings::create();
	// Session
	mSDASession = SDASession::create(mSDASettings);
	//mSDASettings->mCursorVisible = true;
	setUIVisibility(mSDASettings->mCursorVisible);
	mSDASession->getWindowsResolution();
	// OSC
	mReceiver.setListener("/mousemove/1",
		[&](const osc::Message &msg) {
		mCurrentCirclePos.x = msg[0].int32();
		mCurrentCirclePos.y = msg[1].int32();
	});
	mReceiver.setListener("/mouseclick/1",
		[&](const osc::Message &msg) {
		mCurrentSquarePos = vec2(msg[0].flt(), msg[1].flt()) * vec2(getWindowSize());
	});
	// ? is body 0 to 5 
	mReceiver.setListener("/?/*",
		[&](const osc::Message &message) {
		if (message[3].string() == "HandR") {
			float xHandR = message[0].flt();
			float yHandR = message[1].flt();
			mCurrentSquarePos = vec2(xHandR * getWindowWidth() + getWindowWidth() / 2, yHandR * getWindowHeight() + getWindowHeight() / 2);
			stringstream sParams;
			sParams << "{\"params\" :[{\"name\":110,\"value\":" << toString(xHandR);
			sParams << "},{\"name\":111,\"value\":" << toString(yHandR) << "}]}";
			mSDASession->wsWrite(sParams.str());
			CI_LOG_W(sParams.str());
		}
		if (message[3].string() == "HandL") {
			float xHandL = message[0].flt();
			float yHandL = message[1].flt();
			mCurrentCirclePos = vec2(xHandL * getWindowWidth() + getWindowWidth() / 2, yHandL * getWindowHeight() + getWindowHeight() / 2);
			stringstream sParams;
			sParams << "{\"params\" :[{\"name\":112,\"value\":" << toString(xHandL);
			sParams << "},{\"name\":113,\"value\":" << toString(yHandL) << "}]}";
			mSDASession->wsWrite(sParams.str());
			CI_LOG_W(sParams.str());
		}
	});
	try {
		// Bind the receiver to the endpoint. This function may throw.
		mReceiver.bind();
	}
	catch (const osc::Exception &ex) {
		CI_LOG_E("Error binding: " << ex.what() << " val: " << ex.value());
		quit();
	}

	// UDP opens the socket and "listens" accepting any message from any endpoint. The listen
	// function takes an error handler for the underlying socket. Any errors that would
	// call this function are because of problems with the socket or with the remote message.
	mReceiver.listen(
		[](asio::error_code error, protocol::endpoint endpoint) -> bool {
		if (error) {
			CI_LOG_E("Error Listening: " << error.message() << " val: " << error.value() << " endpoint: " << endpoint);
			return false;
		}
		else
			return true;
	});

	mouseGlobal = false;
	mFadeInDelay = true;
	// windows
	mIsShutDown = false;
	/* mRenderWindowTimer = 0.0f;
	timeline().apply(&mRenderWindowTimer, 1.0f, 2.0f).finishFn([&] { positionRenderWindow(); });
	not tested change port https://discourse.libcinder.org/t/resetting-osc-receiver-with-the-new-port/282/9
	long newPort = [[NSUserDefaults standardUserDefaults] integerForKey:@“osc_port”];
	if (mPort != newPort) {
	mPort = newPort;
	mReceiver.reset();
	setupOSCListener(); // does mReceiver = std::make_unique<osc::ReceiverUdp>(mPort);
	} else {
	mReceiver->bind();
	mReceiver->listen();
	}
	*/
}
void SDAServerApp::positionRenderWindow() {
	mSDASettings->mRenderPosXY = ivec2(mSDASettings->mRenderX, mSDASettings->mRenderY);//20141214 was 0
	setWindowPos(mSDASettings->mRenderX, mSDASettings->mRenderY);
	setWindowSize(mSDASettings->mRenderWidth, mSDASettings->mRenderHeight);
}
void SDAServerApp::setUIVisibility(bool visible)
{
	if (visible)
	{
		showCursor();
	}
	else
	{
		hideCursor();
	}
}
void SDAServerApp::fileDrop(FileDropEvent event)
{
	mSDASession->fileDrop(event);
}
void SDAServerApp::update()
{
	mSDASession->setFloatUniformValueByIndex(mSDASettings->IFPS, getAverageFps());
	mSDASession->update();
}
void SDAServerApp::cleanup()
{
	if (!mIsShutDown)
	{
		mIsShutDown = true;
		CI_LOG_V("shutdown");
		// save settings
		mSDASettings->save();
		mSDASession->save();
		quit();
	}
}
void SDAServerApp::mouseMove(MouseEvent event)
{
	if (!mSDASession->handleMouseMove(event)) {
		// let your application perform its mouseMove handling here
	}
}
void SDAServerApp::mouseDown(MouseEvent event)
{
	if (!mSDASession->handleMouseDown(event)) {
		// let your application perform its mouseDown handling here
		if (event.isRightDown()) {
		}
	}
}
void SDAServerApp::mouseDrag(MouseEvent event)
{
	if (!mSDASession->handleMouseDrag(event)) {
		// let your application perform its mouseDrag handling here
	}
}
void SDAServerApp::mouseUp(MouseEvent event)
{
	if (!mSDASession->handleMouseUp(event)) {
		// let your application perform its mouseUp handling here
	}
}

void SDAServerApp::keyDown(KeyEvent event)
{
	if (!mSDASession->handleKeyDown(event)) {
		switch (event.getCode()) {
		case KeyEvent::KEY_ESCAPE:
			// quit the application
			quit();
			break;
		case KeyEvent::KEY_h:
			// mouse cursor and ui visibility
			mSDASettings->mCursorVisible = !mSDASettings->mCursorVisible;
			setUIVisibility(mSDASettings->mCursorVisible);
			break;
		}
	}
}
void SDAServerApp::keyUp(KeyEvent event)
{
	if (!mSDASession->handleKeyUp(event)) {
	}
}

void SDAServerApp::draw()
{
	gl::clear(Color::black());
	if (mFadeInDelay) {
		mSDASettings->iAlpha = 0.0f;
		if (getElapsedFrames() > mSDASession->getFadeInDelay()) {
			mFadeInDelay = false;
			timeline().apply(&mSDASettings->iAlpha, 0.0f, 1.0f, 1.5f, EaseInCubic());
		}
	}

	//gl::setMatricesWindow(toPixels(getWindowSize()),false);
	//gl::setMatricesWindow(getWindowSize());
	gl::setMatricesWindow(mSDASettings->mRenderWidth, mSDASettings->mRenderHeight, false);
	//gl::draw(mSDASession->getMixTexture(), getWindowBounds());
	gl::drawStrokedCircle(mCurrentCirclePos, 100);
	gl::drawSolidRect(Rectf(mCurrentSquarePos - vec2(50), mCurrentSquarePos + vec2(50)));

	// Spout Send
	mSpoutOut.sendViewport();
	getWindow()->setTitle(mSDASettings->sFps + " fps SDAServer");
}

void prepareSettings(App::Settings *settings)
{
#if defined( CINDER_MSW )
	settings->setConsoleWindowEnabled();
#endif
	settings->setMultiTouchEnabled(false);
	settings->setWindowSize(640, 480);
}

CINDER_APP(SDAServerApp, RendererGl, prepareSettings)
