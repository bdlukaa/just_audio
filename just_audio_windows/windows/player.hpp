#pragma comment(lib, "windowsapp")

#include <chrono>

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.System.h>

#define TO_MILLISECONDS(timespan) timespan.count() / 10000
#define TO_MICROSECONDS(timespan) TO_MILLISECONDS(timespan) * 1000

using flutter::EncodableMap;
using flutter::EncodableValue;

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;

// Looks for |key| in |map|, returning the associated value if it is present, or
// a nullptr if not.
//
// The variant types are mapped with Dart types in following ways:
// std::monostate       -> null
// bool                 -> bool
// int32_t              -> int
// int64_t              -> int
// double               -> double
// std::string          -> String
// std::vector<uint8_t> -> Uint8List
// std::vector<int32_t> -> Int32List
// std::vector<int64_t> -> Int64List
// std::vector<float>   -> Float32List
// std::vector<double>  -> Float64List
// EncodableList        -> List
// EncodableMap         -> Map
const EncodableValue* ValueOrNull(const EncodableMap& map, const char* key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &(it->second);
}

// Converts a std::string to std::wstring
auto TO_WIDESTRING = [](std::string string) -> std::wstring {
  if (string.empty()) {
    return std::wstring();
  }
  int32_t target_length =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
                            static_cast<int32_t>(string.length()), nullptr, 0);
  if (target_length == 0) {
    return std::wstring();
  }
  std::wstring utf16_string;
  utf16_string.resize(target_length);
  int32_t converted_length =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
                            static_cast<int32_t>(string.length()),
                            utf16_string.data(), target_length);
  if (converted_length == 0) {
    return std::wstring();
  }
  return utf16_string;
};

class AudioPlayer
{
private:
    /* data */
public:
    std::string id;
    Playback::MediaPlayer mediaPlayer {};
    Playback::MediaPlaybackList mediaPlayerList {};

    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> player_channel_;
    std::unique_ptr<flutter::EventSink<>> event_sink_;
    std::unique_ptr<flutter::EventSink<>> data_sink_;

    AudioPlayer::AudioPlayer(std::string idx, flutter::BinaryMessenger* messenger) {
        id = idx;

        // Set up channels
        player_channel_ =
          std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            messenger, "com.ryanheise.just_audio.methods." + idx,
            &flutter::StandardMethodCodec::GetInstance());

        player_channel_->SetMethodCallHandler(
          [player = this](const auto &call, auto result) {
            player->HandleMethodCall(call, std::move(result));
          });

        event_sink_ = nullptr;
        data_sink_ = nullptr;

        auto events_channel_ = 
          std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
            messenger, "com.ryanheise.just_audio.events." + idx,
            &flutter::StandardMethodCodec::GetInstance());

        auto eventsHandler = std::make_unique<flutter::StreamHandlerFunctions<>>(
          [player = this](const EncodableValue* arguments, std::unique_ptr<flutter::EventSink<>>&& events) -> std::unique_ptr<flutter::StreamHandlerError<>> {
            player->event_sink_ = std::move(events);
            return nullptr;
        },
        [](const EncodableValue* arguments)-> std::unique_ptr<flutter::StreamHandlerError<>> { return nullptr; });

        events_channel_->SetStreamHandler(std::move(eventsHandler));

        auto dataChannel = 
          std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
            messenger, "com.ryanheise.just_audio.data." + idx,
            &flutter::StandardMethodCodec::GetInstance());

        auto dataHandler = std::make_unique<flutter::StreamHandlerFunctions<>>(
          [player = this](const EncodableValue* arguments, std::unique_ptr<flutter::EventSink<>>&& events) -> std::unique_ptr<flutter::StreamHandlerError<>> {
            player->data_sink_ = std::move(events);
            return nullptr;
        },
        [](const EncodableValue* arguments)-> std::unique_ptr<flutter::StreamHandlerError<>> { return nullptr; });

        dataChannel->SetStreamHandler(std::move(dataHandler));

        /// Set up event callbacks
        // Playback event
        mediaPlayer.PlaybackSession().PlaybackStateChanged([=](auto, const auto& args) -> void {
          broadcastPlaybackEvent();
          broadcastDataEvent();
        });

        // Player error event
        mediaPlayer.MediaFailed([=](auto, const Playback::MediaPlayerFailedEventArgs& args) -> void {
          // event_sink_->Error(args.Error().ToString(), args.ErrorMessage());
          std::string errorMessage = winrt::to_string(args.ErrorMessage());
          if (args.Error() == Playback::MediaPlayerError::Unknown) {
            event_sink_->Error("unknown", errorMessage);
          } else if (args.Error() == Playback::MediaPlayerError::Aborted) {
            event_sink_->Error("abort", errorMessage);
          } else if (args.Error() == Playback::MediaPlayerError::NetworkError) {
            event_sink_->Error("networkError", errorMessage);
          } else if (args.Error() == Playback::MediaPlayerError::DecodingError) {
            event_sink_->Error("decodingError", errorMessage);
          } else if (args.Error() == Playback::MediaPlayerError::SourceNotSupported) {
            event_sink_->Error("sourceNotSupported", errorMessage);
          }
        });
    }
    AudioPlayer::~AudioPlayer() {
        mediaPlayer.Close();
    }

    bool HasPlayerId(std::string playerId) {
      return id == playerId;
    }

    void AudioPlayer::HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue> &method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
        // std::cout << method_call.method_name();
        if (method_call.method_name().compare("load") == 0) {
          const auto* audioSourceData = std::get_if<flutter::EncodableMap>(ValueOrNull(*args, "audioSource"));
          // const auto* initialPosition = std::get_if<int32_t>(ValueOrNull(*args, "initialPosition"));
          // const auto* initialIndex = std::get_if<int32_t>(ValueOrNull(*args, "initialIndex"));

          getMediaSource(*audioSourceData);

          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("play") == 0) {
          mediaPlayer.Play();
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("pause") == 0) {
          mediaPlayer.Pause();
          // mediaPlayer.StepForwardOneFrame();
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setVolume") == 0) {
          const auto* volume = std::get_if<double>(ValueOrNull(*args, "volume"));
          if (!volume) {
            return result->Error("volume_error", "volume argument missing");
          }
          float volumeFloat = (float) *volume;
          mediaPlayer.Volume(volumeFloat);
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setSpeed") == 0) {
          const auto* speed = std::get_if<double>(ValueOrNull(*args, "speed"));
          if (!speed) {
            return result->Error("speed_error", "speed argument missing");
          }
          float speedFloat = (float) *speed;
          mediaPlayer.PlaybackRate(speedFloat);
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setPitch") == 0) {
          const auto* pitch = std::get_if<double>(ValueOrNull(*args, "pitch"));
          if (!pitch) {
            return result->Error("pitch_error", "pitch argument missing");
          }
          float pitchFloat = (float) *pitch;
          mediaPlayer.AudioBalance(pitchFloat);
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setSkipSilence") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setLoopMode") == 0) {
          auto* loopMode = std::get_if<int>(ValueOrNull(*args, "loopMode"));
          if (!loopMode) {
            return result->Error("loopMode_error", "loopMode argument missing");
          }

          if (loopMode == (int) 0) {
            // off
            mediaPlayer.IsLoopingEnabled(false);
            mediaPlayerList.AutoRepeatEnabled(false);
          } else if (loopMode == (int*) 1) {
            // one
            mediaPlayer.IsLoopingEnabled(true);
            mediaPlayerList.AutoRepeatEnabled(false);
          } else if (loopMode == (int*) 2) {
            // all
            mediaPlayerList.AutoRepeatEnabled(true);
            mediaPlayer.IsLoopingEnabled(false);
          }


          
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setShuffleMode") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setShuffleOrder") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setAutomaticallyWaitsToMinimizeStalling") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setCanUseNetworkResourcesForLiveStreamingWhilePaused") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setPreferredPeakBitRate") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("seek") == 0) {
          const auto* position = std::get_if<int>(ValueOrNull(*args, "position"));
          
          if (position == nullptr) {
            const auto* position2 = std::get_if<int64_t>(ValueOrNull(*args, "position"));
            if (position2 == nullptr) {
              result->Error("position", "the position argument can not be null");
            } else if (mediaPlayer.CanSeek()) {
              mediaPlayer.Position(TimeSpan(std::chrono::microseconds(*position2)));
              // mediaPlayer.TimelineControllerPositionOffset(TimeSpan(std::chrono::microseconds(*position2)));
              result->Success(flutter::EncodableMap());
            }
          } else if (mediaPlayer.CanSeek()) {
            mediaPlayer.Position(TimeSpan(std::chrono::microseconds(*position)));
            // mediaPlayer.TimelineControllerPositionOffset(TimeSpan(std::chrono::microseconds(*position)));
            result->Success(flutter::EncodableMap());
          }
        } else if (method_call.method_name().compare("concatenatingInsertAll") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("concatenatingRemoveRange") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("concatenatingMove") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("setAndroidAudioAttributes") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("audioEffectSetEnabled") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("androidLoudnessEnhancerSetTargetGain") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("androidEqualizerGetParameters") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("androidEqualizerBandSetGain") == 0) {
          result->Success(flutter::EncodableMap());
        } else if (method_call.method_name().compare("dispose") == 0) {
          mediaPlayer.Close();
          result->Success(flutter::EncodableMap());
        } else {
          result->NotImplemented();
        }
    }

    void AudioPlayer::getMediaSource(const flutter::EncodableMap &source) const& {
      // const std::string* id = std::get_if<std::string>(ValueOrNull(*source, "id"));
      const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
      std::cout << type << std::endl;
      if (type->compare("progressive") == 0) {
        const auto* uri = std::get_if<std::string>(ValueOrNull(source, "uri"));
        // const auto* headers = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "headers"));

        mediaPlayer.SetUriSource(Uri(TO_WIDESTRING(*uri)));
      } else if (type->compare("dash") == 0) {
        const auto* uri = std::get_if<std::string>(ValueOrNull(source, "uri"));
        // const auto* headers = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "headers"));
        mediaPlayer.SetUriSource(Uri(TO_WIDESTRING(*uri)));
      } else if (type->compare("hsl") == 0) {
        const auto* uri = std::get_if<std::string>(ValueOrNull(source, "uri"));
        // const auto* headers = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "headers"));
        mediaPlayer.SetUriSource(Uri(TO_WIDESTRING(*uri)));
      } else if (type->compare("silence") == 0) {
        throw "silence audiosource type is currently not supported";
      } else if (type->compare("concatenating") == 0) {
        // clear the current list
        mediaPlayerList.Items().Clear();

        // mediaPlayerList.

        throw "concatenating audiosource type is currently not supported";
      } else if (type->compare("clipping") == 0) {
        throw "clipping audiosource type is currently not supported";
      } else if (type->compare("looping") == 0) {
        throw "looping audiosource type is currently not supported";
      } else {
        throw std::invalid_argument( "Unknown AudioSource type: " + *type );
      }
    }

    void AudioPlayer::broadcastPlaybackEvent() {
      auto session = mediaPlayer.PlaybackSession();
      auto eventData = flutter::EncodableMap();

      auto duration = TO_MICROSECONDS(session.NaturalDuration());

      auto now = std::chrono::system_clock::now();

      eventData[flutter::EncodableValue("processingState")] = flutter::EncodableValue(processingState(session.PlaybackState()));
      eventData[flutter::EncodableValue("updatePosition")] = flutter::EncodableValue(TO_MICROSECONDS(session.Position())); //int
      eventData[flutter::EncodableValue("updateTime")] = flutter::EncodableValue(TO_MILLISECONDS(now.time_since_epoch())); //int
      // eventData[flutter::EncodableValue("bufferedPosition")] = flutter::EncodableValue(negativeToPositive((int) (duration * session.BufferingProgress()))); //int
      eventData[flutter::EncodableValue("bufferedPosition")] = flutter::EncodableValue((int) 0); //int
      eventData[flutter::EncodableValue("duration")] = flutter::EncodableValue(duration); //int

      event_sink_->Success(eventData);
    }

    int AudioPlayer::processingState(Playback::MediaPlaybackState state) {
      if (state == Playback::MediaPlaybackState::None) {
        return 0; //idle
      } else if (state == Playback::MediaPlaybackState::Opening) {
        return 1; //loading
      } else if (state == Playback::MediaPlaybackState::Buffering) {
        return 2;//buffering
      } else if (mediaPlayer.PlaybackSession().Position().count() ==
                mediaPlayer.PlaybackSession().NaturalDuration().count()) {
        return 4; //completed
      }
      return 3; //ready
    }

    void AudioPlayer::broadcastDataEvent() {
      auto session = mediaPlayer.PlaybackSession();
      auto eventData = flutter::EncodableMap();

      auto isPlaying = session.PlaybackState() == Playback::MediaPlaybackState::Playing;

      eventData[flutter::EncodableValue("playing")] = flutter::EncodableValue(isPlaying);
      eventData[flutter::EncodableValue("volume")] = flutter::EncodableValue(mediaPlayer.Volume());
      eventData[flutter::EncodableValue("speed")] = flutter::EncodableValue(session.PlaybackRate());
      eventData[flutter::EncodableValue("pitch")] = flutter::EncodableValue(mediaPlayer.AudioBalance());
      eventData[flutter::EncodableValue("loopMode")] = flutter::EncodableValue(getLoopMode());
      eventData[flutter::EncodableValue("shuffleMode")] = flutter::EncodableValue(getShuffleMode());

      data_sink_->Success(eventData);
    }

    int AudioPlayer::getLoopMode() {
      // 
      if (!mediaPlayerList.AutoRepeatEnabled() && mediaPlayerList.Items().Size() == 0) {
        return 2;
      }

      if (!mediaPlayer.IsLoopingEnabled()) {
        return 0;
      }

      return 1;
    }

    int AudioPlayer::getShuffleMode() {
      if (mediaPlayerList.ShuffleEnabled()) {
        return 1;
      }

      return 0;
    }

    flutter::EncodableMap AudioPlayer::collectIcyMetadata() {
      auto icyData = flutter::EncodableMap();

      // TODO: Icy Metadata
      // mediaPlayer.PlaybackMediaMarkers();

      return icyData;
    }

    /// Transforms a num into positive, if negative
    int negativeToPositive(int num) {
      if (num < 0) { return num * (-1); }
      return num;
    }

}; 
