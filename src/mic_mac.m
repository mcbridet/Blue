/* Whether macOS lets Blue use the microphone (see audio.h): AVFoundation's authorization for audio capture */
#import <AVFoundation/AVFoundation.h>

#include "audio.h"

int audio_mic_permission(void)
{
	static bool asked;
	switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
	case AVAuthorizationStatusAuthorized:
		return 1;
	case AVAuthorizationStatusNotDetermined:
		/* (the system asks the user, with Info.plist's NSMicrophoneUsageDescription; tried again later) */
		if (!asked)
			[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
				(void)granted;
			}];
		asked = true;
		return -1;
	default:
		return 0;
	}
}
