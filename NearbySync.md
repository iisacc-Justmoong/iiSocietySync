# BLE 발견과 TCP 파일 전송 (0.6)

`BleDiscovery`와 `NearbyBootstrap`은 가까운 Society 기기의 연결 정보를 교환한다. 파일 복제·재개·충돌·삭제·SHA-256 검증은 `Controller`/`Replica`가 계속 담당한다. 앱은 SDK가 발견한 후보를 기존 계정 페어링에 전달한다.

## 연결 순서

1. BLE 광고에는 `a91f1130-7ab5-4e10-92a0-642309ef0001` 서비스 UUID만 넣는다.
2. 같은 서비스의 읽기 전용 GATT 특성 네 개(`…0002`~`…0005`)에서 연결 정보를 읽는다. 각 특성은 최대 500 bytes, 전체는 2,000 bytes이다. SHA-256은 갱신 도중 서로 다른 페이지를 읽었는지 확인하며 인증 수단은 아니다.
3. 연결 정보에는 허용된 발견 레코드, 최대 8개 사설 IPv4 주소, 초대 UDP 포트만 있다. 계정 cookie·비밀키·파일·파일명은 싣지 않는다. 일반 LAN 발견과 같은 공개 scope/회전 proof를 사용한다.
4. 소비자는 같은 계정의 HMAC, 양쪽 nonce, 대상 ID, 초대 만료를 확인하고 TLS 인증서 지문을 고정한다. BLE 발견만으로 접근 권한이 생기지 않는다.
5. 파일은 Wi-Fi/LAN의 인증된 TCP/TLS WebSocket으로만 전송한다. 인터넷 연결은 필요하지 않지만 최초 계정 로그인과 페어링 권한 준비는 필요하다.

```cpp
iiSocietySync::BleDiscovery discovery;
QObject::connect(&discovery, &iiSocietySync::BleDiscovery::found,
    receiver, &Receiver::untrustedEndpointDiscovered);
discovery.start(signedDiscoveryRecord, invitationUdpPort);
// 로그아웃 / 컨테이너 소유권 이전 / OS 실행 시간 만료
discovery.stop();
```

Society의 `HybridDiscoveryService`가 BLE와 기존 Bonjour/NSD 후보를 `NearbyDevices`에 전달한다. 계정 필터와 페어링 정책은 두 발견 경로에서 동일하다. BLE 권한 거부·하드웨어 부재가 LAN 발견을 막지 않는다. `status()`/`statusChanged()`로 BLE 상태를 조회한다. 5초 스캔을 12초 간격으로 실행하고 최대 16개 연결 후보, 한 번에 한 GATT 연결, 6초 연결 제한을 적용한다. 앱이 실행 중일 때만 동작하며 새로운 백그라운드 실행 권한을 만들지 않는다.

## 파일 전송 윈도우와 바이너리

호스트 `describe.transferWindow=4`를 협상한다. 각 파일에서 256 KiB 조각 최대 4개를 동시에 요청한다. 내려받은 응답은 오프셋 순으로 저장하고, 업로드는 TCP 요청 순서를 유지하며 순서가 바뀐 ACK를 모아 연속 오프셋만 확정한다. 중복·늦은 응답은 다시 적용하지 않는다. 요청별 120초 제한과 전송 세대 번호가 취소된 응답을 격리한다. `setTransferWindow(1..4)`는 유휴 `Synchronizer`에서만 변경할 수 있다. 구형 호스트는 윈도우 1로 낮춘다.

파일바이트 윈도우는 최대 1 MiB이며 JSON 표현·호스트 작업 캐시·소켓 버퍼는 별도이다. 약 1초마다 **진행 중인 윈도우를 비운 뒤** 매니페스트를 갱신하므로 작은 수정과 삭제가 대형 파일 전체를 기다리지 않는다. 원자적 설치 전에 완료 크기와 SHA-256을 검증한다.

iiServerHost 0.6 `LanPeer`끼리는 기존 TLS 페어링에서 `binary=1`을 협상한다. 파일 `data`만 원시 bytes로 보내므로 256 KiB 본문에 필요한 약 85 KiB의 Base64 증가분을 제거한다. 내부 `QJsonObject` API는 유지하므로 내부 Base64 변환 비용까지 제거한 구현은 아니다. 제어 메시지·구형 peer·인터넷 relay는 기존 JSON을 유지한다. 바이너리 입력도 TLS와 페어링을 완료한 세션에서만 수락한다.

## 플랫폼과 직접 연결 결정

Qt 6.8.3 Bluetooth 백엔드를 사용한다. macOS/iOS는 CoreBluetooth, Android는 해당 OS Bluetooth 백엔드를 사용한다. CMake 기본값 `IISOCIETYSYNC_WITH_BLE=ON`은 Bluetooth 모듈이 없으면 구성을 실패시킨다. Bluetooth가 필요 없는 서버는 명시적으로 `OFF`로 빌드할 수 있다. Windows에서는 Qt의 peripheral 역할을 지원하지 않아 BLE 스캔과 기존 LAN 발견을 사용한다. `IISOCIETYSYNC_DISABLE_BLE=1`은 테스트·headless 환경의 라디오 사용을 끈다.

Apple 앱에는 `NSBluetoothAlwaysUsageDescription`, Android에는 scan/connect/advertise 권한과 구형 OS의 위치 권한 선언을 추가한다. 런타임 권한은 Qt의 permission API로 요청한다. OS 권한 거부 상태에서는 LAN 경로를 사용한다.

현재 전송 구현은 **TCP/TLS**이다. QUIC는 이 동기화 프로토콜에 구현하지 않았으며 다른 FileTransfer 백엔드의 HTTP/3 지원을 Society QUIC 지원으로 표시하지 않는다.

공유 AP가 없는 직접 연결은 검토했으나 이번 경로에 섞지 않았다. Apple의 `kDNSServiceFlagsIncludeP2P`는 P2P 탐색을 포함할 뿐 기존 사설 IPv4 전용 `LanPeer`가 AWDL의 scoped IPv6를 전송할 수 있게 하지 않는다. Android Wi-Fi Direct는 그룹 생성·권한·경로 수명 관리가 따로 필요하고 Apple AWDL과 동일한 프로토콜이 아니다. 따라서 이번 버전은 인터넷 없는 **공유 Wi-Fi/LAN**을 지원하며, AP 없는 AWDL/Wi-Fi Direct 그룹 자동 생성과 QUIC를 지원한다고 광고하지 않는다.

근거: [Qt peripheral 플랫폼 제약](https://doc.qt.io/qt-6/qlowenergycontroller.html), [Apple P2P 탐색 플래그](https://developer.apple.com/documentation/dnssd/kdnsserviceflagsincludep2p), [Android Wi-Fi Direct](https://developer.android.com/develop/connectivity/wifi/wifi-direct), [Android Bluetooth 권한](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions).

## 검증

`Bootstrap`은 메타데이터 상한·페이지 오염·비밀정보 필드 거부를, `Synchronizer`는 지연/역순/중복 응답·구형 호스트 협상·작은 변경 우선 처리를 검사한다. `SocketTransfer`는 실제 loopback TCP/TLS에서 8 MiB 이상 파일의 다운로드 중단·재개·업로드·SHA-256·접근 철회를 검사한다. iiServerHost의 LAN 테스트는 바이너리 프레임 크기·양방향 소켓 전송·구형 JSON 클라이언트를 검사한다.

자동 테스트는 실제 무선 스캔을 켜지 않는다. 빌드와 소켓 테스트는 BLE 무선 발견·GATT 교환의 실기기 성공 증거가 아니며, 실제 무선 처리량과 절전 상태 복귀는 별도 두 기기 검증이 필요하다.
