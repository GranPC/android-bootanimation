/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BootAnimation"

#include <stdint.h>
#include <sys/types.h>
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>

#include <cutils/properties.h>

#include <androidfw/AssetManager.h>
#include <binder/IPCThreadState.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/threads.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/DisplayInfo.h>
#include <ui/FramebufferNativeWindow.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <core/SkBitmap.h>
#include <core/SkStream.h>
#include <core/SkImageDecoder.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/eglext.h>

#include "BootAnimation.h"

#define USER_BOOTANIMATION_FILE "/data/local/bootanimation.zip"
#define SYSTEM_BOOTANIMATION_FILE "/system/media/bootanimation.zip"
#define SYSTEM_ENCRYPTED_BOOTANIMATION_FILE "/system/media/bootanimation-encrypted.zip"
#define EXIT_PROP_NAME "service.bootanim.exit"

extern "C" int clock_nanosleep(clockid_t clock_id, int flags,
						   const struct timespec *request,
						   struct timespec *remain);

namespace android {

// ---------------------------------------------------------------------------

BootAnimation::BootAnimation() : Thread(false)
{
	mSession = new SurfaceComposerClient();
}

BootAnimation::~BootAnimation() {
}

void BootAnimation::onFirstRef() {
	status_t err = mSession->linkToComposerDeath(this);
	ALOGE_IF(err, "linkToComposerDeath failed (%s) ", strerror(-err));
	if (err == NO_ERROR) {
		run("BootAnimation", PRIORITY_DISPLAY);
	}
}

sp<SurfaceComposerClient> BootAnimation::session() const {
	return mSession;
}


void BootAnimation::binderDied(const wp<IBinder>& who)
{
	// woah, surfaceflinger died!
	ALOGD("SurfaceFlinger died, exiting...");

	// calling requestExit() is not enough here because the Surface code
	// might be blocked on a condition variable that will never be updated.
	kill( getpid(), SIGKILL );
	requestExit();
}

status_t BootAnimation::initTexture(Texture* texture, AssetManager& assets,
		const char* name) {
	// Asset* asset = assets.open(name, Asset::ACCESS_BUFFER);
	FILE *f = fopen( name, "rb" );
	if (!f)
		return NO_INIT;
	fseek( f, 0L, SEEK_END );
	long size = ftell( f );
	fseek( f, 0L, SEEK_SET );

	char *buf = ( char * ) malloc( size );
	fread( buf, size, 1, f );
	SkBitmap bitmap;
	SkImageDecoder::DecodeMemory( (void *) buf, size,
			&bitmap, SkBitmap::kNo_Config, SkImageDecoder::kDecodePixels_Mode);
	fclose( f );
	free( buf );
	// asset->close();
	// delete asset;

	// ensure we can call getPixels(). No need to call unlock, since the
	// bitmap will go out of scope when we return from this method.
	bitmap.lockPixels();

	const int w = bitmap.width();
	const int h = bitmap.height();
	const void* p = bitmap.getPixels();

	GLint crop[4] = { 0, h, w, -h };
	texture->w = w * ( 1.50f * 2 );
	texture->h = h * ( 1.50f * 2 );

	glGenTextures(1, &texture->name);
	glBindTexture(GL_TEXTURE_2D, texture->name);

	switch (bitmap.getConfig()) {
		case SkBitmap::kA8_Config:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA,
					GL_UNSIGNED_BYTE, p);
			break;
		case SkBitmap::kARGB_4444_Config:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
					GL_UNSIGNED_SHORT_4_4_4_4, p);
			break;
		case SkBitmap::kARGB_8888_Config:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
					GL_UNSIGNED_BYTE, p);
			break;
		case SkBitmap::kRGB_565_Config:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
					GL_UNSIGNED_SHORT_5_6_5, p);
			break;
		default:
			break;
	}

	glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	return NO_ERROR;
}

status_t BootAnimation::initTexture(void* buffer, size_t len)
{
	//StopWatch watch("blah");

	SkBitmap bitmap;
	SkMemoryStream  stream(buffer, len);
	SkImageDecoder* codec = SkImageDecoder::Factory(&stream);
	codec->setDitherImage(false);
	if (codec) {
		codec->decode(&stream, &bitmap,
				SkBitmap::kARGB_8888_Config,
				SkImageDecoder::kDecodePixels_Mode);
		delete codec;
	}

	// ensure we can call getPixels(). No need to call unlock, since the
	// bitmap will go out of scope when we return from this method.
	bitmap.lockPixels();

	const int w = bitmap.width();
	const int h = bitmap.height();
	const void* p = bitmap.getPixels();

	GLint crop[4] = { 0, h, w, -h };
	int tw = 1 << (31 - __builtin_clz(w));
	int th = 1 << (31 - __builtin_clz(h));
	if (tw < w) tw <<= 1;
	if (th < h) th <<= 1;

	switch (bitmap.getConfig()) {
		case SkBitmap::kARGB_8888_Config:
			if (tw != w || th != h) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
						GL_UNSIGNED_BYTE, 0);
				glTexSubImage2D(GL_TEXTURE_2D, 0,
						0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
			} else {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
						GL_UNSIGNED_BYTE, p);
			}
			break;

		case SkBitmap::kRGB_565_Config:
			if (tw != w || th != h) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB,
						GL_UNSIGNED_SHORT_5_6_5, 0);
				glTexSubImage2D(GL_TEXTURE_2D, 0,
						0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
			} else {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB,
						GL_UNSIGNED_SHORT_5_6_5, p);
			}
			break;
		default:
			break;
	}

	glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);

	return NO_ERROR;
}

status_t BootAnimation::readyToRun() {
	mAssets.addDefaultAssets();

	sp<IBinder> dtoken(SurfaceComposerClient::getBuiltInDisplay(
			ISurfaceComposer::eDisplayIdMain));
	DisplayInfo dinfo;
	status_t status = SurfaceComposerClient::getDisplayInfo(dtoken, &dinfo);
	if (status)
		return -1;

	// create the native surface
	sp<SurfaceControl> control = session()->createSurface(String8("BootAnimation"),
			dinfo.w, dinfo.h, PIXEL_FORMAT_RGBA_8888);

	SurfaceComposerClient::openGlobalTransaction();
	control->setLayer(0x40000000);
	SurfaceComposerClient::closeGlobalTransaction();

	sp<Surface> s = control->getSurface();

	// initialize opengl and egl
	const EGLint attribs[] = {
			EGL_RED_SIZE,   8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE,  8,
			EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 0,
			EGL_NONE
	};
	EGLint w, h, dummy;
	EGLint numConfigs;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

	eglInitialize(display, 0, 0);
	eglChooseConfig(display, attribs, &config, 1, &numConfigs);
	surface = eglCreateWindowSurface(display, config, s.get(), NULL);
	context = eglCreateContext(display, config, NULL, NULL);
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);

	if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE)
		return NO_INIT;

	mDisplay = display;
	mContext = context;
	mSurface = surface;
	mWidth = w;
	mHeight = h;
	mFlingerSurfaceControl = control;
	mFlingerSurface = s;

	mAndroidAnimation = true;

	return NO_ERROR;
}

bool BootAnimation::threadLoop()
{
	bool r;
	r = android();

	// No need to force exit anymore
	property_set(EXIT_PROP_NAME, "0");

	eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(mDisplay, mContext);
	eglDestroySurface(mDisplay, mSurface);
	mFlingerSurface.clear();
	mFlingerSurfaceControl.clear();
	eglTerminate(mDisplay);
	IPCThreadState::self()->stopProcess();
	return r;
}

bool BootAnimation::android()
{
	// maybe i should hardcode this texture.
	initTexture( &mAndroid[0], mAssets, "/data/local/plus.png" );

	float vtxcoords[] = { 0, 0, mAndroid[ 0 ].w, 0, 0, mAndroid[ 0 ].h, mAndroid[ 0 ].w, mAndroid[ 0 ].h };

	float texcoords[] = {
		0, 0,
		1, 0,
		0, 1,
		1, 1,
	};

	// clear screen
	glShadeModel(GL_FLAT);
	glDisable(GL_DITHER);
	glDisable(GL_SCISSOR_TEST);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0, ( float ) mWidth, ( float ) mHeight, 0, -1, 1 );

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vtxcoords);
	glTexCoordPointer(2, GL_FLOAT, 0, texcoords);

	double flStandardCol = 1;
	double flInvertCol = 0.05;
	bool bInverted = false;

	glClearColor( bInverted ? flStandardCol : flInvertCol, bInverted ? flStandardCol : flInvertCol, bInverted ? flStandardCol : flInvertCol, 1 );
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(mDisplay, mSurface);

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, mAndroid[0].name);
	glEnable(GL_BLEND);
	glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	float hw = mAndroid[ 0 ].w / 2.f;
	float hh = mAndroid[ 0 ].h / 2.f;

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	nsecs_t startTime = systemTime();
	nsecs_t fadeTime;
	double nextInvert = -1;

	bool fading = false;
	bool shouldExit = false;

	do {
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		nsecs_t now = systemTime();
		double time = now - startTime;

		if ( nextInvert != -1 && time >= nextInvert )
		{
			startTime = now;
			time = now - startTime;
			bInverted = !bInverted;
		}

		float angle = float( time / us2ns( 8333 / 1.65f ) );

		if ( angle >= 90 )
		{
			angle = 90;
			if ( nextInvert == -1 )
				nextInvert = time + us2ns( 55000 );
		}

		double alpha = 1;

		if ( fading )
		{
			alpha = 1 - ( ( now - fadeTime ) / ( 500000000.f ));

			if ( alpha < -0.f ) shouldExit = true;
		}

		double reverseAlpha = (1 - alpha) * 3;

		float flMainCol = (!bInverted ? flStandardCol : flInvertCol) - reverseAlpha;
		float flBgCol = (bInverted ? flStandardCol : flInvertCol) - reverseAlpha;

		glColor4f( flMainCol, flMainCol, flMainCol, alpha );
		glClearColor( flBgCol, flBgCol, flBgCol, alpha );
		glClear( GL_COLOR_BUFFER_BIT );

		for ( int x = 0; x < mWidth * 4.f; x += mAndroid[ 0 ].w )
		{
			for ( int y = 0; y < mHeight * 4.f; y += mAndroid[ 0 ].h )
			{
				int xChange = ( ( y / mAndroid[ 0 ].h ) % 3 ) * ( mAndroid[ 0 ].w - mAndroid[ 0 ].w / 3 - mAndroid[ 0 ].w / 3 );
				int yChange = ( ( x / mAndroid[ 0 ].w ) % 3 ) * ( mAndroid[ 0 ].h / 3 );
				int xChange2 = y / mAndroid[ 0 ].h / 3;
				int x3 = ( ( x / mAndroid[ 0 ].w ) / 3 ) * mAndroid[ 0 ].w;

				xChange -= x3 * ( 2.f / 3.f );
				yChange -= x3 * ( 1.f / 3.f );

				if ( ( x / mAndroid[ 0 ].w ) % 3 != 0 )
				{
					int finalX = x + xChange + xChange2 * mAndroid[ 0 ].w - mWidth * 2.f - 1;
					int finalY = y - yChange - mHeight * 2.f - 1;

					if ( bInverted )
					{
						finalX -= mAndroid[ 0 ].w / 3.f + mAndroid[ 0 ].h / 3.f;
						finalY -= mAndroid[ 0 ].h / 3.f;
					}

					if ( finalX < mWidth || finalY < mHeight )
					{
						if ( finalX > -mAndroid[ 0 ].w && finalY > -mAndroid[ 0 ].h )
						{ 
							glPushMatrix();
								glTranslatef( finalX + hw, finalY + hh, 0 );
								glRotatef( angle, 0.0f, 0.0f, ( bInverted ? 1 : -1 ) );
								glTranslatef( -hw, -hh, 0 );
								glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
							glPopMatrix();
						}
					}
				}
			}
		}

		EGLBoolean res = eglSwapBuffers(mDisplay, mSurface);
		if (res == EGL_FALSE)
			break;

		// 12fps: don't animate too fast to preserve CPU
		const nsecs_t sleepTime = 8333 - ns2us(systemTime() - now);
		if (sleepTime > 0)
			usleep(sleepTime);

		checkExit();

		if ( (false || exitPending()) && !fading )
		{
			fadeTime = systemTime();
			fading = true;
		}
	} while ( !shouldExit );

	glDeleteTextures(1, &mAndroid[0].name);
	return false;
}


void BootAnimation::checkExit() {
	// Allow surface flinger to gracefully request shutdown
	char value[PROPERTY_VALUE_MAX];
	property_get(EXIT_PROP_NAME, value, "0");
	int exitnow = atoi(value);
	if (exitnow) {
		requestExit();
	}
}

// ---------------------------------------------------------------------------

}
; // namespace android
