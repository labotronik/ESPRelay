import 'dart:convert';

Map<String, dynamic>? parseBleState(String payload) {
  try {
    final value = jsonDecode(payload);
    if (value is Map<String, dynamic>) return value;
  } catch (_) {}
  return null;
}

class BleStateFrame {
  const BleStateFrame(this.payload, this.state);
  final String payload;
  final Map<String, dynamic> state;
}

class BleStateFramer {
  final List<int> _buffer = [];
  void clear() => _buffer.clear();
  List<BleStateFrame> add(List<int> bytes) {
    _buffer.addAll(bytes);
    final frames = <BleStateFrame>[];
    while (_buffer.isNotEmpty) {
      var start = -1;
      for (var i = 0; i + 1 < _buffer.length; i++) {
        if (_buffer[i] == 0x1e && _buffer[i + 1] == 0x1e) {
          start = i;
          break;
        }
      }
      if (start < 0) {
        final trailingStart = _buffer.last == 0x1e;
        _buffer.clear();
        if (trailingStart) _buffer.add(0x1e);
        break;
      }
      if (start > 0) _buffer.removeRange(0, start);
      final end = _buffer.indexOf(0x1f, 2);
      if (end < 0) {
        if (_buffer.length > 16384) _buffer.clear();
        break;
      }
      if (end <= 16384) {
        try {
          final payload = utf8.decode(_buffer.sublist(2, end));
          final state = parseBleState(payload);
          if (state != null) frames.add(BleStateFrame(payload, state));
        } catch (_) {/* Skip an invalid UTF-8 frame and accept the next one. */}
      }
      _buffer.removeRange(0, end + 1);
    }
    return frames;
  }
}

String bleControlCommand(String target, int index, String action) {
  if (target == 'I' && index == 0 && action.isEmpty) return 'IDENTIFY';
  final validActions = target == 'R'
      ? const ['ON', 'OFF', 'AUTO', 'TOGGLE']
      : const ['ON', 'OFF', 'TOGGLE'];
  if (!['R', 'V'].contains(target) || index < 1 || index > 16 ||
      !validActions.contains(action)) {
    throw ArgumentError('Invalid BLE command');
  }
  return '$target$index:$action';
}
