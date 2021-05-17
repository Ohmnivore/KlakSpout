﻿//
//		SpoutFrameCount
//
//		Semaphore frame counter
//
// ====================================================================================
//		Revisions :
//
//		07.10.18	- project start
//		01.11.18	- Add GetRefreshRate() to set default sender fps to system refresh rate
//		16.11.18	- Profile UpdateSenderFps
//		23.11.18	- Change semaphore access functions to operate within a mutex lock
//		26.11.18	- Add application disable frame counting to avoid variable frame rate problems
//		27.11.18	- Add IsFrameNew
//		02.12.18	- Add sender GetWidth, GetHeight, GetFps and GetFrame
//		23.12.18	- More log warnings
//		26.02.19	- Add IsFrameCountEnabled
//		14.03.19	- CleanupFrameCount - return if no semaphore handle
//					- Remove wait warning from CheckAccess()
//		02.04.19	- Profile timing functions
//		24.04.19	- Add HoldFps
//		19.05.19	- Clean up
//		05.06.19	- HoldFps - use std::chrono if VS2015 or greater
//		03.03.20	- Introduce DX11 keyed mutex locks in addition to named mutex
//		11.03.20	- General cleanup
//					  Result switch for WaitForSingleObject
//		05.05.20	- Mutex access timing tests documented within functions
//		18.06.20	- Update comments
//		06.09.20	- Add more notice logs to EnableFrameCount
//		23.09.20	- Initialize m_lastFrame, m_FrameStart, m_bIsNewFrame
//		24.09.20	- Remove m_FrameStartPtr and m_FrameEndPtr null checks in destructor
//		14.12.10	- independent std::chrono timing for sender frame count and HoldFps
//					  Testing and code optimization
//		18.12.20	- Add SetFrameCount for registry setting
//		04.02.21	- Reset timers in EnableFrameCount
//
// ====================================================================================
//
/*
	Copyright (c) 2019-2021. Lynn Jarvis. All rights reserved.

	Redistribution and use in source and binary forms, with or without modification, 
	are permitted provided that the following conditions are met:

		1. Redistributions of source code must retain the above copyright notice, 
		   this list of conditions and the following disclaimer.

		2. Redistributions in binary form must reproduce the above copyright notice, 
		   this list of conditions and the following disclaimer in the documentation 
		   and/or other materials provided with the distribution.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"	AND ANY 
	EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
	OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE	ARE DICLAIMED. 
	IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
	PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
	LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "SpoutFrameCount.h"

//
// Class: spoutFrameCount
//
// Semaphore frame counter.
//
// Refer to source code for documentation.
//

// -----------------------------------------------
spoutFrameCount::spoutFrameCount()
{
	m_hAccessMutex = NULL;
	m_hCountSemaphore = NULL;
	m_SenderName[0] = 0;
	m_CountSemaphoreName[0] = 0;
	m_FrameCount = 0L;
	m_LastFrameCount = 0L;
	m_FrameTimeTotal = 0.0;
	m_FrameTimeNumber = 0.0;
	m_lastFrame = 0.0;
	m_FrameStart = 0.0;
	m_bIsNewFrame = false;
	m_SenderFps = GetRefreshRate(); // Default sender fps is system refresh rate

	// Check the registry setting for frame counting between sender and receiver
	m_bFrameCount = false; // default not set
	DWORD dwFrame = 0;
	if (ReadDwordFromRegistry(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", "Framecount", &dwFrame)) {
		m_bFrameCount = (dwFrame == 1);
	}
	m_bDisabled = false; // frame counting not application disabled

	// Initialize fps control
	m_millisForFrame = 0.0;

#ifdef USE_CHRONO
	// Start std::chrono microsec counting
	m_FrameStartPtr = new std::chrono::steady_clock::time_point;
	m_FrameEndPtr = new std::chrono::steady_clock::time_point;
	m_FramePtr = new std::chrono::steady_clock::time_point;
#else
	// Initialize PC msec frequency counter
	PCFreq = 0.0;
	CounterStart = 0;
	StartCounter();
#endif

}

// -----------------------------------------------
spoutFrameCount::~spoutFrameCount()
{

#ifdef USE_CHRONO
	delete m_FrameStartPtr;
	delete m_FrameEndPtr;
	delete m_FramePtr;
#endif

	// Close the frame count semaphore.
	if (m_hCountSemaphore) CloseHandle(m_hCountSemaphore);
	m_hCountSemaphore = NULL;

	// Close the texture access mutex
	if (m_hAccessMutex) CloseHandle(m_hAccessMutex);
	m_hAccessMutex = NULL;

}


// ======================================================================
//								Public
// ======================================================================

// Enable or disable frame counting globally by registry setting
void spoutFrameCount::SetFrameCount(bool bEnable)
{
	if (bEnable) {
		// Frame counting not already set to registry
		if (!m_bFrameCount) {
			WriteDwordToRegistry(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", "Framecount", 1);
			m_bFrameCount = true;
			m_bDisabled = false; // Application disable flag
		}
		// Do nothing if already set
	}
	else {
		// Frame counting already set to registry
		if (m_bFrameCount) {
			// Clean up existing objects
			if (IsFrameCountEnabled())
				CleanupFrameCount();
		}
		WriteDwordToRegistry(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", "Framecount", 0);
		m_bFrameCount = false;
		m_bDisabled = false;
	}
}

// -----------------------------------------------
//
// Create a frame counting semaphore.
//
// Incremented by a sender.
// Tested by a receiver to retrieve the count.
//
void spoutFrameCount::EnableFrameCount(const char* SenderName)
{
	// Return if frame counting not recorded in the registry
	// Subsequently SetNewFrame and GetNewFrame return without action
	if (!m_bFrameCount) {
		SpoutLogNotice("SpoutFrameCount::EnableFrameCount : setting not enabled");
		return;
	}

	// Return if application disabled
	if (m_bDisabled) {
		SpoutLogNotice("SpoutFrameCount::EnableFrameCount : application disabled");
		return;
	}

	// A sender name is required
	if (SenderName[0] == 0) {
		SpoutLogWarning("SpoutFrameCount::EnableFrameCount - no sender name");
		return;
	}

	// Reset frame count, comparator and fps variables
	m_FrameCount = 0L;
	m_LastFrameCount = 0L;
	m_FrameTimeTotal = 0.0;
	m_FrameTimeNumber = 0.0;
	m_SenderFps = GetRefreshRate();

	// Reset timers
	m_millisForFrame = 0.0;
#ifdef USE_CHRONO
	// Start std::chrono microsec counting
	m_FrameStartPtr = new std::chrono::steady_clock::time_point;
	m_FrameEndPtr = new std::chrono::steady_clock::time_point;
	m_FramePtr = new std::chrono::steady_clock::time_point;
#else
	// Initialize PC msec frequency counter
	PCFreq = 0.0;
	CounterStart = 0;
	StartCounter();
#endif

	// Return if already enabled for this sender
	if (m_hCountSemaphore && strcmp(SenderName, m_SenderName) == 0) {
		SpoutLogNotice("SpoutFrameCount::EnableFrameCount already enabled [%s]", SenderName);
		return;
	}

	SpoutLogNotice("SpoutFrameCount::EnableFrameCount : sender name [%s]", SenderName);

	// Close any existing semaphore
	if (m_hCountSemaphore) {
		CloseHandle(m_hCountSemaphore);
		m_hCountSemaphore = NULL;
		m_CountSemaphoreName[0] = 0;
	}

	// Set the new name for subsequent checks
	strcpy_s(m_SenderName, 256, SenderName);

	// Create or open a semaphore with this sender name
	sprintf_s(m_CountSemaphoreName, 256, "%s_Count_Semaphore", SenderName);
	HANDLE hSemaphore = CreateSemaphoreA(
		NULL, // default security attributes
		1, // initial count
		LONG_MAX, // maximum count - LONG_MAX (2147483647) at 60fps = 2071 days
		(LPSTR)m_CountSemaphoreName);

	DWORD dwError = GetLastError();
	if (dwError == ERROR_INVALID_HANDLE) {
		SpoutLogError("    Invalid semaphore handle");
		return;
	}
	if (dwError == ERROR_ALREADY_EXISTS) {
		SpoutLogNotice("    Semaphore already exists");
		// OK if it already exists - the sender or receiver can create it
	}
	if (hSemaphore == NULL) {
		SpoutLogError("    Unknown error");
		return;
	}

	m_hCountSemaphore = hSemaphore;

	SpoutLogNotice("    Semaphore handle [0x%.7X]", LOWORD(m_hCountSemaphore) );

}

// -----------------------------------------------
void spoutFrameCount::DisableFrameCount()
{
	CleanupFrameCount();
	m_bDisabled = true;
}

// -----------------------------------------------
bool spoutFrameCount::IsFrameCountEnabled()
{
	if (!m_bFrameCount || m_bDisabled)
		return false;
	else
		return true;

}

// -----------------------------------------------
//
// Increment the sender frame count.
// Used by a sender for every update of the shared texture.
//
// This function is called within a sender mutex lock so that
// the receiver will not read the semaphore count while the
// sender is incrementing it.
//
// Used internaly to set frame status if frame counting is enabled.
//
void spoutFrameCount::SetNewFrame()
{
	// Return silently if disabled
	if (!m_bFrameCount || m_bDisabled)
		return;

	// Access the frame count semaphore
	// Note: WaitForSingle object will always succeed because
	// the lock count (sender frame count) is greater than zero,
	// but must be called before ReleaseSemaphore can be called
	// or there is an error.
	DWORD dwWaitResult = WaitForSingleObject(m_hCountSemaphore, 0);
	switch (dwWaitResult) {
		case WAIT_OBJECT_0:
			// Release the frame counting semaphore to increase it's count.
			// so that the receiver can retrieve the new count.
			// Increment by 2 because WaitForSingleObject decremented it.
			if (ReleaseSemaphore(m_hCountSemaphore, 2, NULL) == false) {
				SpoutLogError("spoutFrameCount::SetNewFrame - ReleaseSemaphore failed");
			}
			else {
				m_FrameCount++; // Increment the sender frame count
				// Update the sender fps calculations for the new frame
				UpdateSenderFps(1);
			}
			return;
		case WAIT_ABANDONED:
			SpoutLogError("SpoutFrameCount::SetNewFrame - WAIT_ABANDONED");
			break;
		case WAIT_FAILED:
			SpoutLogError("SpoutFrameCount::SetNewFrame - WAIT_FAILED");
			break;
		default:
			break;
	}
}

// -----------------------------------------------
//
// Read the semaphore count to determine if the sender
// has produced a new frame and incremented the counter.
// Counts are recorded as class variables for a receiver.
//
// This function is called within a sender mutex lock so that
// the sender will not write a frame and increment the 
// count while a receiver is reading it.
//
// Used internally to check frame status if frame counting is enabled.
//
bool spoutFrameCount::GetNewFrame()
{
	long framecount = 0;

	// Return silently if disabled
	if (!m_bFrameCount || m_bDisabled)
		return true;

	// A receiver creates of opens a named semaphore when it connects to a sender
	// Do not block if semaphore creation failed so that ReceiveTexture can still be called
	if (!m_hCountSemaphore) {
		printf("No count sempaphore\n");
		return true;
	}

	// Access the frame count semaphore
	DWORD dwWaitResult = WaitForSingleObject(m_hCountSemaphore, 0);
	switch (dwWaitResult) {
		case WAIT_OBJECT_0:
			// Call ReleaseSemaphore with a release count of 1 to return it
			// to what it was before the wait and record the previous count.
			// The next time round it will either be the same count because
			// the receiver released it, or increased because the sender
			// released and incremented it.
			if (ReleaseSemaphore(m_hCountSemaphore, 1, &framecount) == false) {
				SpoutLogError("spoutFrameCount::GetNewFrame - ReleaseSemaphore failed");
				return true; // do not block
			}
			break;
		case WAIT_ABANDONED :
			SpoutLogWarning("SpoutFrameCount::GetNewFrame - WAIT_ABANDONED");
			break;
		case WAIT_FAILED :
			SpoutLogWarning("SpoutFrameCount::GetNewFrame - WAIT_FAILED");
			break;
		default :
			break;
	}

	// Update the global frame count
	m_FrameCount = framecount;

	// Count will still be zero for apps that do not set a frame count
	if (framecount == 0)
		return true;

	// If this count and the last are the same, the sender has not
	// produced a new frame and incremented the counter.
	// Only return false if this frame and the last are the same.
	if (framecount == m_LastFrameCount) {
		m_bIsNewFrame = false;
		return false;
	}

	// Update the sender fps calculations.
	// The sender might have produced more than one frame if the receiver is slower.
	// Pass the number of frames produced since the last.
	UpdateSenderFps(framecount - m_LastFrameCount);

	// Reset the comparator
	m_LastFrameCount = framecount;

	// Signal a new frame
	m_bIsNewFrame = true;

	return true;
}


// -----------------------------------------------
void spoutFrameCount::CleanupFrameCount()
{

	// Return if no count semaphore
	// i.e. no sender started or cleanup already done
	if (!m_hCountSemaphore)
		return;

	SpoutLogNotice("SpoutFrameCount::CleanupFrameCount");

	// Close the frame count semaphore. If another application first
	// opened the semaphore it will not be finally closed here.
	CloseHandle(m_hCountSemaphore);
	m_hCountSemaphore = NULL;

	// Clear the sender name in case the same one opens again
	m_SenderName[0] = 0;

	// Reset counters
	m_FrameCount = 0L;
	m_LastFrameCount = 0L;
	m_SenderFps = GetRefreshRate();
	m_FrameTimeTotal = 0.0;
	m_FrameTimeNumber = 0.0;

}

// -----------------------------------------------
//
//  Is the received frame new ?
//
//  This function can be used by a receiver after ReceiveTexture
//	to determine whether the frame just received is new.
//
//	Used by an application to avoid time consuming processing
//	after receiving a texture.
//
//	Not usually required because new frame status is always 
//	checked internally if frame counting is enabled.
//
bool spoutFrameCount::IsFrameNew()
{
	return m_bIsNewFrame;
}

// -----------------------------------------------
double spoutFrameCount::GetSenderFps()
{
	return m_SenderFps;
}

// -----------------------------------------------
long spoutFrameCount::GetSenderFrame()
{
	return m_FrameCount;
}

// -----------------------------------------------
//
// Fps control
//
// Not necessary if the application already has frame rate control.
// Must be called every frame.
// The sender will then signal a new frame at the target rate.
// Purpose is control rather than accuracy.
// Use std::chrono if supported by the compiler VS2015 or greater
//
void spoutFrameCount::HoldFps(int fps)
{
	// Return if incorrect fps entry
	if (fps <= 0)
		return;

	double framerate = static_cast<double>(fps);

#ifdef USE_CHRONO
	// Initialize frame time at target rate
	if (m_millisForFrame < 0.01) {
		m_millisForFrame = 1000.0 / framerate; // msec per frame
		*m_FrameStartPtr = std::chrono::steady_clock::now();
		SpoutLogNotice("spoutFrameCount::HoldFps(%d)", fps);
	}
	else {
		*m_FrameEndPtr = std::chrono::steady_clock::now();
		// milliseconds elapsed
		double elapsedTime = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(*m_FrameEndPtr - *m_FrameStartPtr).count() / 1000.);
		// Sleep to reach the target frame time
		if (elapsedTime < m_millisForFrame) { // milliseconds
			std::this_thread::sleep_for(std::chrono::milliseconds((long)(m_millisForFrame - elapsedTime)));
		}
		// Set start time for the next frame
		*m_FrameStartPtr = std::chrono::steady_clock::now();
	}
#else
	if (m_millisForFrame == 0) {
		m_millisForFrame = 1000.0 / framerate;
		m_FrameStart = GetCounter();
		SpoutLogNotice("spoutFrameCount::HoldFps(%d)", fps);
	}
	else {
		double elapsedTime = GetCounter() - m_FrameStart; // msec
		// Sleep to reach the target frame time
		if (elapsedTime < m_millisForFrame)
			Sleep((DWORD)(m_millisForFrame - elapsedTime)); // can be slighly high
		// Set start time for the next frame
		m_FrameStart = GetCounter();
	}
#endif

}

// =================================================================
//                     Texture access mutex
// =================================================================

//
// Check access to the shared texture
//
// Use a keyed mutex if the DX11 texture supports it
// otherwise use the sender named mutex.
// The DX11 texture pointer argument should always be null for DX9 mode.
bool spoutFrameCount::CheckTextureAccess(ID3D11Texture2D* D3D11texture)
{
	if (IsKeyedMutex(D3D11texture)) {
		return CheckKeyedAccess(D3D11texture);
	}
	else {
		return CheckAccess();
	}
}

void spoutFrameCount::AllowTextureAccess(ID3D11Texture2D* D3D11texture)
{
	if (IsKeyedMutex(D3D11texture))
		AllowKeyedAccess(D3D11texture);
	else
		AllowAccess();
}

// -----------------------------------------------
bool spoutFrameCount::CreateAccessMutex(const char *SenderName)
{
	DWORD errnum = 0;
	char szMutexName[512];
	HANDLE hMutex = NULL;

	// Create the mutex name to control access to the shared texture
	sprintf_s((char*)szMutexName, 512, "%s_SpoutAccessMutex", SenderName);

	// Create or open mutex depending, on whether it already exists or not
	//  - A sender will create one.
	//  - A receiver will open for a specific sender.
	//    A receiver should not open a mutex until a sender is found to connect to.
	//    If that sender does not have a mutex, one will be created
	//    and will always be available to the receiver.
	//
	hMutex = CreateMutexA(NULL, false, (LPCSTR)szMutexName);

	if (hMutex == NULL) {
		SpoutLogError("spoutFrameCount::CreateAccessMutex - NULL invalid handle");
		return false;
	}
	else {
		errnum = GetLastError();
		if (errnum == ERROR_INVALID_HANDLE) {
			SpoutLogError("spoutFrameCount::CreateAccessMutex - [%s] invalid handle", szMutexName);
			return false;
		}
		// Here we can find if the mutex already exists
		else if (errnum == ERROR_ALREADY_EXISTS) {
			SpoutLogNotice("spoutFrameCount::CreateAccessMutex - [%s] already exists", szMutexName);
		}
		else {
			SpoutLogNotice("spoutFrameCount::CreateAccessMutex - [%s] created - 0x%.7X", szMutexName, PtrToUint(hMutex) );
		}
	}

	m_hAccessMutex = hMutex;

	return true;

}

// -----------------------------------------------
void spoutFrameCount::CloseAccessMutex()
{
	// Close the texture access mutex. If another application first opened
	// the mutex it will not be finally closed here.
	if (m_hAccessMutex) CloseHandle(m_hAccessMutex);
	m_hAccessMutex = NULL;
}

// -----------------------------------------------
// Check whether any other process is holding the lock
// and wait for access for up to 4 frames if so.
// For receiving from Version 1 apps with no mutex lock,
// a reader will have created the mutex and will have
// sole access and rely on the interop locks.
bool spoutFrameCount::CheckAccess()
{
	// Don't block if no mutex for Spout1 apps
	// or if called when the sender has closed.
	// AllowAccess also tests for a null handle.
	if (!m_hAccessMutex)
		return true;

	// Typically 2-3 microseconds.
	// 10 receivers - no increase.
	// Note that NVIDIA "Threaded optimization" can cause delay
	// for WaitForSingleObject and is set OFF by SpoutSettinngs

	DWORD dwWaitResult = WaitForSingleObject(m_hAccessMutex, 67); // timeout 4 frames at 60fps
	switch (dwWaitResult) {
		case WAIT_OBJECT_0 :
			// The state of the object is signalled.
			return true;
		case WAIT_ABANDONED:
			SpoutLogError("spoutFrameCount::CheckAccess - WAIT_ABANDONED");
			break;
		case WAIT_TIMEOUT: // The time-out interval elapsed, and the object's state is nonsignaled.
			// This can happen the first time a receiver connects to a sender
			// SpoutLogError("CheckAccess - WAIT_TIMEOUT");
			break;
		case WAIT_FAILED: // Could use call GetLastError
			SpoutLogError("spoutFrameCount::CheckAccess - WAIT_FAILED");
			break;
		default:
			SpoutLogError("spoutFrameCount::CheckAccess - unknown error");
			break;
	}
	return false;
}

// -----------------------------------------------
// Release named mutex
void spoutFrameCount::AllowAccess()
{
	// < 1 microsecond
	// Release ownership of the mutex object.
	// The caller must call ReleaseMutex once for each time that the mutex satisfied a wait.
	// The ReleaseMutex function fails if the caller does not own the mutex object
	if (m_hAccessMutex)
		ReleaseMutex(m_hAccessMutex);

}


// ===============================================================================
//                                Protected
// ===============================================================================


// Keyed mutex check
//
// Microsoft docs :
// When a surface is created using the D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX 
// value of the D3D10_RESOURCE_MISC_FLAG enumeration, you must call the 
// AcquireSync method before rendering to the surface. You must call the 
// ReleaseSync method when you are done rendering to a surface.
//
// Tests show that if a DX11 texture has been created with a keyed mutex it must
// be used in place of the sender named mutex or CopyResource fails
//
bool spoutFrameCount::CheckKeyedAccess(ID3D11Texture2D* pTexture)
{
	// 85-90 microseconds
	if (pTexture) {

		IDXGIKeyedMutex* pDXGIKeyedMutex = nullptr;

		// Check the keyed mutex
		pTexture->QueryInterface(_uuidof(IDXGIKeyedMutex), (void**)&pDXGIKeyedMutex);
		if (pDXGIKeyedMutex) {
			HRESULT hr = pDXGIKeyedMutex->AcquireSync(0, 67); // TODO - link with SPOUT_WAIT_TIMEOUT
			switch (hr) {
				case WAIT_OBJECT_0:
					// The state of the object is signalled.
					pDXGIKeyedMutex->Release();
					return true;
				case WAIT_ABANDONED:
					SpoutLogError("spoutDirectX::CheckKeyedAccess : WAIT_ABANDONED");
					break;
				case WAIT_TIMEOUT: // The time-out interval elapsed, and the object's state is nonsignaled.
					SpoutLogError("spoutDirectX::CheckKeyedAccess : WAIT_TIMEOUT");
					break;
				default:
					break;
			}
			pDXGIKeyedMutex->ReleaseSync(0);
			pDXGIKeyedMutex->Release();
		}
	}
	return false;
}

// Release keyed mutex
void spoutFrameCount::AllowKeyedAccess(ID3D11Texture2D* pTexture)
{
	// 22-24 microseconds
	if (pTexture) {
		IDXGIKeyedMutex* pDXGIKeyedMutex;
		pTexture->QueryInterface(_uuidof(IDXGIKeyedMutex), (void**)&pDXGIKeyedMutex);
		if (pDXGIKeyedMutex) {
			pDXGIKeyedMutex->ReleaseSync(0);
			pDXGIKeyedMutex->Release();
		}
	}
}

bool spoutFrameCount::IsKeyedMutex(ID3D11Texture2D* D3D11texture)
{
	// Approximately 1.5 microseconds
	if (D3D11texture) {
		D3D11_TEXTURE2D_DESC desc;
		D3D11texture->GetDesc(&desc);
		if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
			// printf("IsKeyed()\n");
			return true;
		}
	}
	// Return to access by another method if no keyed mutex
	return false;
}


// -----------------------------------------------
// Calculate the sender frames per second
// Applications before 2.007 have a frame rate dependent on the system fps
void spoutFrameCount::UpdateSenderFps(long framecount) {

	// 0.0005 msec per frame

	// If framecount is zero, the sender has not produced a new frame yet
	if (framecount > 0) {

#ifdef USE_CHRONO
		*m_FrameEndPtr = std::chrono::steady_clock::now();
		// milliseconds elapsed
		double frametime = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(*m_FrameEndPtr - *m_FramePtr).count() / 1000.);
		// Reset the frame time
		*m_FramePtr = std::chrono::steady_clock::now();
#else
		// Msecs between this frame and the last
		double thisFrame = GetCounter();
		double frametime = thisFrame-m_lastFrame;
		// Set the start time for the next frame
		m_lastFrame = thisFrame;
#endif
		// Accumulate totals
		m_FrameTimeTotal = m_FrameTimeTotal + frametime;
		m_FrameTimeNumber += (double)framecount; // Could have been more than one frame
		if (m_FrameTimeNumber > 16) {
			// Calculate the average frame time every 16 frames
			frametime = m_FrameTimeTotal / m_FrameTimeNumber;
			m_FrameTimeTotal = 0.0;
			m_FrameTimeNumber = 0.0;
			// Calculate frames per second (default fps is system refresh rate)
			frametime = frametime / 1000.0; // frame time in seconds
			if (frametime > 0.0001) {
				double fps = (1.0 / frametime); // Fps
				m_SenderFps = 0.85*m_SenderFps + 0.15*fps; // damping
			}
		}
	}

}

// -----------------------------------------------
//
// Get system refresh rate for the default fps value
// https://docs.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-enumdisplaysettingsa
//
double spoutFrameCount::GetRefreshRate()
{
	double frequency = 60.0;
	DEVMODE DevMode;
	BOOL bResult = true;
	DWORD dwCurrentSettings = 0;
	DevMode.dmSize = sizeof(DEVMODE);
	// Test all the graphics modes
	while (bResult) {
		bResult = EnumDisplaySettings(NULL, dwCurrentSettings, &DevMode);
		if (bResult)
			frequency = static_cast<double>(DevMode.dmDisplayFrequency);
		dwCurrentSettings++;
	}
	return frequency;
}

// -----------------------------------------------
//
// Set counter start
//
// Information on using QueryPerformanceFrequency for timing
// https://docs.microsoft.com/en-us/windows/desktop/SysInfo/acquiring-high-resolution-time-stamps
//
// Used instead of std::chrono for Visual Studio before VS2015
//
void spoutFrameCount::StartCounter()
{
	LARGE_INTEGER li;
	if (QueryPerformanceFrequency(&li)) {
		// Find the PC frequency if not done yet
		if(PCFreq < 0.0001)
			PCFreq = static_cast<double>(li.QuadPart) / 1000.0;
		// Get the counter start
		QueryPerformanceCounter(&li);
		CounterStart = li.QuadPart;
	}

}

// -----------------------------------------------
// Return msec elapsed since counter start
//
double spoutFrameCount::GetCounter()
{
	LARGE_INTEGER li;
	if (QueryPerformanceCounter(&li)) {
		return static_cast<double>(li.QuadPart - CounterStart) / PCFreq;
	}
	else {
		return 0.0;
	}
}

// ===============================================================================


