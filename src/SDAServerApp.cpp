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
#include "cinder/TriMesh.h"
#include "cinder/Camera.h"
#include "cinder/Rand.h"
#include "cinder/ImageIo.h"
#include "cinder/gl/TransformFeedbackObj.h"
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
#define SLIDER_NOTE 1
//#define USE_UDP 1

#if USE_UDP
using Receiver = osc::ReceiverUdp;
using protocol = asio::ip::udp;
#else
using Receiver = osc::ReceiverTcp;
using protocol = asio::ip::tcp;
#endif

const uint16_t localPort = 7000;
const int nParticles = 4000;
const int PositionIndex = 0;
const int VelocityIndex = 1;
const int StartTimeIndex = 2;
const int InitialVelocityIndex = 3;

float mix(float x, float y, float a)
{
	return x * (1 - a) + y * a;
}
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
	//// Session
	SDASessionRef					mSDASession;
	//// Log
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
	// particles
	void							loadBuffers();
	void							loadShaders();
	void							loadTexture();
	gl::VaoRef						mPVao[2];
	gl::TransformFeedbackObjRef		mPFeedbackObj[2];
	gl::VboRef						mPPositions[2], mPVelocities[2], mPStartTimes[2], mPInitVelocity;

	gl::GlslProgRef					mPUpdateGlsl, mPRenderGlsl;
	gl::TextureRef					mSmokeTexture;

	Rand							mRand;
	CameraPersp						mCam;
	TriMeshRef						mTrimesh;
	uint32_t						mDrawBuff;
};


SDAServerApp::SDAServerApp()
	: mSpoutOut("SDAServer", app::getWindowSize()),
	mReceiver(localPort)
{
	// Settings
	mSDASettings = SDASettings::create();
	// Session
	mSDASession = SDASession::create(mSDASettings);
	gl::disableDepthRead();
	gl::disableDepthWrite();
	//mSDASettings->mCursorVisible = true;
	setUIVisibility(mSDASettings->mCursorVisible);
	//mSDASession->getWindowsResolution();
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
	try {
		// Bind the receiver to the endpoint. This function may throw.
		mReceiver.bind();
	}
	catch (const osc::Exception &ex) {
		CI_LOG_E("Error binding: " << ex.what() << " val: " << ex.value());
		quit();
	}

#if USE_UDP
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
#else
	mReceiver.setConnectionErrorFn(
		// Error Function for Accepted Socket Errors. Will be called anytime there's an
		// error reading from a connected socket (a socket that has been accepted below).
		[&](asio::error_code error, uint64_t identifier) {
		if (error) {
			auto foundIt = mConnections.find(identifier);
			if (foundIt != mConnections.end()) {
				// EOF or end of file error isn't specifically an error. It's just that the
				// other side closed the connection while you were expecting to still read.
				if (error == asio::error::eof) {
					CI_LOG_W("Other side closed the connection: " << error.message() << " val: " << error.value() << " endpoint: " << foundIt->second.address().to_string()
						<< " port: " << foundIt->second.port());
				}
				else {
					CI_LOG_E("Error Reading from Socket: " << error.message() << " val: "
						<< error.value() << " endpoint: " << foundIt->second.address().to_string()
						<< " port: " << foundIt->second.port());
				}
				mConnections.erase(foundIt);
			}
		}
	});
	auto expectedOriginator = protocol::endpoint(asio::ip::address::from_string("127.0.0.1"), 10000);
	mReceiver.accept(
		// Error Handler for the acceptor. You'll return true if you want to continue accepting
		// or fals otherwise.
		[](asio::error_code error, protocol::endpoint endpoint) -> bool {
		if (error) {
			CI_LOG_E("Error Accepting: " << error.message() << " val: " << error.value()
				<< " endpoint: " << endpoint.address().to_string());
			return false;
		}
		else
			return true;
	},
		// Accept Handler. Return whether or not the acceptor should cache this connection
		// (true) or dismiss it (false).
		[&, expectedOriginator](osc::TcpSocketRef socket, uint64_t identifier) -> bool {
		// Here we return whether or not the remote endpoint is the expected endpoint
		mConnections.emplace(identifier, socket->remote_endpoint());
		return socket->remote_endpoint() == expectedOriginator;
	});
#endif

	// particles
	mDrawBuff = 1;

	mCam.setPerspective(60.0f, getWindowAspectRatio(), .01f, 1000.0f);
	mCam.lookAt(vec3(0, 0, 10), vec3(0, 0, 0));

	loadTexture();
	loadShaders();
	loadBuffers();
	// windows
	mouseGlobal = false;
	mFadeInDelay = true;
	mIsShutDown = false;
	mRenderWindowTimer = 0.0f;
	timeline().apply(&mRenderWindowTimer, 1.0f, 2.0f).finishFn([&] { positionRenderWindow(); });
}
void SDAServerApp::positionRenderWindow() {
	mSDASettings->mRenderPosXY = ivec2(mSDASettings->mRenderX, mSDASettings->mRenderY);//20141214 was 0
	setWindowPos(mSDASettings->mRenderX, mSDASettings->mRenderY);
	setWindowSize(mSDASettings->mRenderWidth, mSDASettings->mRenderHeight);
}
void SDAServerApp::loadTexture()
{
	gl::Texture::Format mTextureFormat;
	mTextureFormat.magFilter(GL_LINEAR).minFilter(GL_LINEAR).mipmap().internalFormat(GL_RGBA);
	mSmokeTexture = gl::Texture::create(loadImage(loadAsset("smoke_blur.png")), mTextureFormat);
}
void SDAServerApp::loadShaders()
{
	try {
		// Create a vector of Transform Feedback "Varyings".
		// These strings tell OpenGL what to look for when capturing
		// Transform Feedback data. For instance, Position, Velocity,
		// and StartTime are variables in the updateSmoke.vert that we
		// write our calculations to.
		std::vector<std::string> transformFeedbackVaryings(3);
		transformFeedbackVaryings[PositionIndex] = "Position";
		transformFeedbackVaryings[VelocityIndex] = "Velocity";
		transformFeedbackVaryings[StartTimeIndex] = "StartTime";

		ci::gl::GlslProg::Format mUpdateParticleGlslFormat;
		// Notice that we don't offer a fragment shader. We don't need
		// one because we're not trying to write pixels while updating
		// the position, velocity, etc. data to the screen.
		mUpdateParticleGlslFormat.vertex(loadAsset("updateSmoke.vert"))
			// This option will be either GL_SEPARATE_ATTRIBS or GL_INTERLEAVED_ATTRIBS,
			// depending on the structure of our data, below. We're using multiple
			// buffers. Therefore, we're using GL_SEPERATE_ATTRIBS
			.feedbackFormat(GL_SEPARATE_ATTRIBS)
			// Pass the feedbackVaryings to glsl
			.feedbackVaryings(transformFeedbackVaryings)
			.attribLocation("VertexPosition", PositionIndex)
			.attribLocation("VertexVelocity", VelocityIndex)
			.attribLocation("VertexStartTime", StartTimeIndex)
			.attribLocation("VertexInitialVelocity", InitialVelocityIndex);

		mPUpdateGlsl = ci::gl::GlslProg::create(mUpdateParticleGlslFormat);
	}
	catch (const ci::gl::GlslProgCompileExc &ex) {
		console() << "PARTICLE UPDATE GLSL ERROR: " << ex.what() << std::endl;
	}

	mPUpdateGlsl->uniform("H", 1.0f / 60.0f);
	mPUpdateGlsl->uniform("Accel", vec3(0.0f));
	mPUpdateGlsl->uniform("ParticleLifetime", 3.0f);

	try {
		ci::gl::GlslProg::Format mRenderParticleGlslFormat;
		// This being the render glsl, we provide a fragment shader.
		mRenderParticleGlslFormat.vertex(loadAsset("renderSmoke.vert"))
			.fragment(loadAsset("renderSmoke.frag"))
			.attribLocation("VertexPosition", PositionIndex)
			.attribLocation("VertexStartTime", StartTimeIndex);

		mPRenderGlsl = ci::gl::GlslProg::create(mRenderParticleGlslFormat);
	}
	catch (const ci::gl::GlslProgCompileExc &ex) {
		console() << "PARTICLE RENDER GLSL ERROR: " << ex.what() << std::endl;
	}

	mPRenderGlsl->uniform("ParticleTex", 0);
	mPRenderGlsl->uniform("MinParticleSize", 1.0f);
	mPRenderGlsl->uniform("MaxParticleSize", 64.0f);
	mPRenderGlsl->uniform("ParticleLifetime", 3.0f);
}

void SDAServerApp::loadBuffers()
{
	// Initialize positions
	std::vector<vec3> positions(nParticles, vec3(0.0f));

	// Create Position Vbo with the initial position data
	mPPositions[0] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, positions.size() * sizeof(vec3), positions.data(), GL_STATIC_DRAW);
	// Create another Position Buffer that is null, for ping-ponging
	mPPositions[1] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, positions.size() * sizeof(vec3), nullptr, GL_STATIC_DRAW);

	// Reuse the positions vector that we've already made
	std::vector<vec3> normals = std::move(positions);

	for (auto normalIt = normals.begin(); normalIt != normals.end(); ++normalIt) {
		// Creating a random velocity for each particle in a unit sphere
		*normalIt = ci::randVec3() * mix(0.0f, 1.5f, mRand.nextFloat());
	}

	// Create the Velocity Buffer using the newly buffered velocities
	mPVelocities[0] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, normals.size() * sizeof(vec3), normals.data(), GL_STATIC_DRAW);
	// Create another Velocity Buffer that is null, for ping-ponging
	mPVelocities[1] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, normals.size() * sizeof(vec3), nullptr, GL_STATIC_DRAW);
	// Create an initial velocity buffer, so that you can reset a particle's velocity after it's dead
	mPInitVelocity = ci::gl::Vbo::create(GL_ARRAY_BUFFER, normals.size() * sizeof(vec3), normals.data(), GL_STATIC_DRAW);

	// Create time data for the initialization of the particles
	array<GLfloat, nParticles> timeData;
	float time = 0.0f;
	float rate = 0.001f;
	for (int i = 0; i < nParticles; i++) {
		timeData[i] = time;
		time += rate;
	}

	// Create the StartTime Buffer, so that we can reset the particle after it's dead
	mPStartTimes[0] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, timeData.size() * sizeof(float), timeData.data(), GL_DYNAMIC_COPY);
	// Create the StartTime ping-pong buffer
	mPStartTimes[1] = ci::gl::Vbo::create(GL_ARRAY_BUFFER, nParticles * sizeof(float), nullptr, GL_DYNAMIC_COPY);

	for (int i = 0; i < 2; i++) {
		// Initialize the Vao's holding the info for each buffer
		mPVao[i] = ci::gl::Vao::create();

		// Bind the vao to capture index data for the glsl
		mPVao[i]->bind();
		mPPositions[i]->bind();
		ci::gl::vertexAttribPointer(PositionIndex, 3, GL_FLOAT, GL_FALSE, 0, 0);
		ci::gl::enableVertexAttribArray(PositionIndex);

		mPVelocities[i]->bind();
		ci::gl::vertexAttribPointer(VelocityIndex, 3, GL_FLOAT, GL_FALSE, 0, 0);
		ci::gl::enableVertexAttribArray(VelocityIndex);

		mPStartTimes[i]->bind();
		ci::gl::vertexAttribPointer(StartTimeIndex, 1, GL_FLOAT, GL_FALSE, 0, 0);
		ci::gl::enableVertexAttribArray(StartTimeIndex);

		mPInitVelocity->bind();
		ci::gl::vertexAttribPointer(InitialVelocityIndex, 3, GL_FLOAT, GL_FALSE, 0, 0);
		ci::gl::enableVertexAttribArray(InitialVelocityIndex);

		// Create a TransformFeedbackObj, which is similar to Vao
		// It's used to capture the output of a glsl and uses the
		// index of the feedback's varying variable names.
		mPFeedbackObj[i] = gl::TransformFeedbackObj::create();

		// Bind the TransformFeedbackObj and bind each corresponding buffer
		// to it's index.
		mPFeedbackObj[i]->bind();
		gl::bindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, PositionIndex, mPPositions[i]);
		gl::bindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, VelocityIndex, mPVelocities[i]);
		gl::bindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, StartTimeIndex, mPStartTimes[i]);
		mPFeedbackObj[i]->unbind();
	}
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
	//mSDASession->fileDrop(event);
}
void SDAServerApp::update()
{
	mSDASession->setFloatUniformValueByIndex(mSDASettings->IFPS, getAverageFps());
	mSDASession->update();
	// This equation just reliably swaps all concerned buffers
	mDrawBuff = 1 - mDrawBuff;

	gl::ScopedGlslProg	glslScope(mPUpdateGlsl);
	// We use this vao for input to the Glsl, while using the opposite
	// for the TransformFeedbackObj.
	gl::ScopedVao		vaoScope(mPVao[mDrawBuff]);
	// Because we're not using a fragment shader, we need to
	// stop the rasterizer. This will make sure that OpenGL won't
	// move to the rasterization stage.
	gl::ScopedState		stateScope(GL_RASTERIZER_DISCARD, true);

	mPUpdateGlsl->uniform("Time", getElapsedFrames() / 60.0f);

	// Opposite TransformFeedbackObj to catch the calculated values
	// In the opposite buffer
	mPFeedbackObj[1 - mDrawBuff]->bind();

	// We begin Transform Feedback, using the same primitive that
	// we're "drawing". Using points for the particle system.
	gl::beginTransformFeedback(GL_POINTS);
	gl::drawArrays(GL_POINTS, 0, nParticles);
	gl::endTransformFeedback();
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
	
}
void SDAServerApp::mouseDown(MouseEvent event)
{
	
}
void SDAServerApp::mouseDrag(MouseEvent event)
{
	
}
void SDAServerApp::mouseUp(MouseEvent event)
{
	
}

void SDAServerApp::keyDown(KeyEvent event)
{
	
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
void SDAServerApp::keyUp(KeyEvent event)
{
	
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
	gl::draw(mSDASession->getMixTexture(), Rectf(0.0f, 0.0f, 100.0f, 100.0f));
	gl::drawStrokedCircle(mCurrentCirclePos, 100);
	
	static float rotateRadians = 0.0f;
	rotateRadians += 0.01f;

	gl::ScopedVao			vaoScope(mPVao[1 - mDrawBuff]);
	gl::ScopedGlslProg		glslScope(mPRenderGlsl);
	gl::ScopedTextureBind	texScope(mSmokeTexture);
	gl::ScopedState			stateScope(GL_PROGRAM_POINT_SIZE, true);
	gl::ScopedBlend			blendScope(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	gl::pushMatrices();
	gl::setMatrices(mCam);
	gl::multModelMatrix(rotate(rotateRadians, vec3(0, 1, 0)));

	mPRenderGlsl->uniform("Time", getElapsedFrames() / 60.0f);
	gl::setDefaultShaderVars();
	gl::drawArrays(GL_POINTS, 0, nParticles);

	gl::popMatrices();
	// Spout Send
	mSpoutOut.sendViewport();
	getWindow()->setTitle(mSDASettings->sFps + " fps SDA");
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
