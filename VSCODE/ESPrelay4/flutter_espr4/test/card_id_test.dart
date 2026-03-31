import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_espr4/main.dart';

void main() {
  test('extract card id from MAC with separators', () {
    expect(extractCardIdFromAny('BC:D5:38:7D:78:FE'), 'BCD5387D78FE');
  });

  test('extract card id from plain hex', () {
    expect(extractCardIdFromAny('bcd5387d78fe'), 'BCD5387D78FE');
  });
}
