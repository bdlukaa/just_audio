
import 'dart:async';

import 'package:flutter/services.dart';

class JustAudioWindows {
  static const MethodChannel _channel = MethodChannel('just_audio_windows');

  static Future<String?> get platformVersion async {
    final String? version = await _channel.invokeMethod('getPlatformVersion');
    return version;
  }
}
