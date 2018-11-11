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
	// kinect
	/*string jointNames[26] = { "SpineBase", "SpineMid", "Neck", "Head",
		"ShldrL", "ElbowL", "WristL", "HandL",
		"ShldrR", "ElbowR", "WristR", "HandR",
		"HipL", "KneeL", "AnkleL", "FootL",
		"HipR", "KneeR", "AnkleR", "FootR",
		"SpineShldr", "HandTipL", "ThumbL", "HandTipR", "ThumbR", "Count" };*/
	map<int, vec4>					mJoints;
	ivec2							mLeftHandPos;
	vec2							mRightHandPos;
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
	mReceiver.setListener("/link/",
		[&](const osc::Message &message) {
		float bpm = (float)message[0].dbl();
		float beat = (float)message[1].dbl();
		float phase = (float)message[2].dbl();
		stringstream sParams;
		sParams << "{\"params\" :[{\"name\":" << toString(mSDASettings->IBPM) << ",\"value\":" << toString(bpm);
		sParams << "},{\"name\":" << toString(mSDASettings->ITIME) << ",\"value\":" << toString(beat);
		sParams << "},{\"name\":" << toString(mSDASettings->IPHASE) << ",\"value\":" << toString(phase) << "}]}";
		mSDASession->wsWrite(sParams.str());
		CI_LOG_W(sParams.str());

	});
	mReceiver.setListener("/mouseclick/1",
		[&](const osc::Message &message) {
		mRightHandPos = vec2(message[0].flt(), message[1].flt()) * vec2(getWindowSize());
	});
	mReceiver.setListener("/kV2/body/?",
		[&](const osc::Message &message) {
		CI_LOG_W(message[0]);
	});
	mReceiver.setListener("/kv2status",
		[&](const osc::Message &message) {
		CI_LOG_W(message[0]);
		mSDASession->wsWrite(message[0].string());

	});

	// ? is body 0 to 5 
	mReceiver.setListener("/?/*",
		[&](const osc::Message &message) {
		float x = message[0].flt();
		float y = message[1].flt();
		float z = message[2].flt();
		int jointIndex = message[3].int32() + 200;
		int bodyIndex = message[4].int32();
		string jointName = message[5].string();
		stringstream sParams;
		sParams << "{\"k2\" :[{\"name\":\"" << toString(jointIndex) << "\",\"value\":\"" << toString(x) << "," << toString(y) << "," << toString(z) << "," << toString(bodyIndex) << "\"}]}";
		mSDASession->wsWrite(sParams.str());
		if (jointIndex == 211) CI_LOG_W(sParams.str());
		mJoints[message[3].int32()] = vec4(x * getWindowWidth() + getWindowWidth() / 2, 
			y * getWindowHeight() + getWindowHeight() / 2, z, bodyIndex);
		
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
	//gl::drawStrokedCircle(mLeftHandPos, 100);
	//gl::drawStrokedCircle(mRightHandPos, 100);
	//gl::drawSolidRect(Rectf(mRightHandPos - vec2(50), mRightHandPos + vec2(50)));
	//mRightHandPos = vec2(xHandR * getWindowWidth() + getWindowWidth() / 2, yHandR * getWindowHeight() + getWindowHeight() / 2);
	
	for (unsigned i = 0; i < mJoints.size(); ++i) 
	{		
		gl::drawSolidCircle(mJoints[i], 10);
	}
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
