# iiSocietySync 0.3.0

서로 다른 기기에서 실행 중인 Society의 컨테이너 데이터를 동기화하는 C++20/Qt SDK이다. 같은 계정의 인증된 Society 연결을 받아 변경 감지, 양방향 전송, 중단 복구, 충돌 보존을 수행한다. `helloWorld()`는 기존 소비자 호환용으로 유지한다.

## 책임과 의존 방향

| 구성 요소 | 담당하는 일 | 의존 경계 |
| --- | --- | --- |
| iiSocietyHelper | 같은 기기의 앱 관측, 메시지·ACK, 객체 스냅샷, 계정 모델 참조, 로컬 공통 파일 접근 | Container·Account를 사용하며 Sync·ServerHost에 의존하지 않는다. |
| iiSocietySync | 다른 기기의 Society 컨테이너 복제, 원격 Files 탐색·다운로드 | Container·ServerHost를 사용하며 Helper·제품 앱에 의존하지 않는다. |
| iiSocietyContainer | 컨테이너 UUID, 8개 영역, 로컬 공유·OS 제공자 | 동기화 프로토콜을 알지 않는다. |
| iiServerHost | 인증된 일반 요청·응답, LAN TLS·기존 relay, 제한된 파일 서비스 | Society 변경 기록·충돌 정책을 알지 않는다. |
| Society 앱 | 로그인·그룹 상태, 계정 증명, 발견, 자동 페어링 큐, 호스트 선택, 앱 수명·화면 | 인증된 기기 ID와 계정 scope, 현재 컨테이너·전송 콜백을 Sync에 제공한다. |

기존 Qt 6.8.3 Core/Network/Sql, SQLite와 iiServerHost 0.4.1의 TLS를 재사용한다. 새 외부 라이브러리·서버·유료 서비스를 추가하지 않는다. 해시에는 Qt의 [QCryptographicHash](https://doc.qt.io/qt-6.8/qcryptographichash.html), 프로세스 간 작업 배제에는 [QLockFile](https://doc.qt.io/qt-6.8/qlockfile.html), 영속 저널에는 [SQLite WAL](https://sqlite.org/wal.html)을 사용한다. 유지보수·라이선스는 기존 Qt/SQLite/SDK 구성의 조건을 따른다. Society 고유의 버전·충돌 정책만 이 SDK에서 구현한다.

## 공개 API

- `Controller`: 전용 작업 스레드, 컨테이너 열기·닫기, 인증된 기기 집합, 순차 동기화, 원격 요청 처리.
- `Replica`: 단일 스레드의 로컬 컨테이너·SQLite 변경 저널. `scan`, `changes`, `record`, `handle`, 기기별 커서 영속화.
- `Synchronizer`: 한 원격 Society와 한 회차의 양방향 전송. `requestReady`와 `receive`로 전송 구현을 분리한다.
- `RemoteFiles`, `filesHandler`: 원격 Files 탐색·원자적 다운로드와 Files 전용 서비스. 전체 컨테이너 복제는 별도 인증된 `society.sync` 프로토콜이다.

Windows의 `filesHandler`는 Sync의 네이티브 파일 접근을 사용해 Files 목록·메타데이터·읽기·새 파일/디렉터리 생성을 제공한다. 업로드는 비공개 임시 파일을 완성한 뒤 덮어쓰기 없는 hard link로 공개한다. 같은 이름이 이미 있으면 기존 파일을 보존하고 거부한다. 범용 iiServerHost의 POSIX FileShare를 Windows에서 호출하거나 ServerHost가 Sync를 역참조하지 않는다. 이 Files 투영은 Models 등 다른 영역을 열지 않으며 전체 컨테이너 변경은 Replica 프로토콜로 처리한다.

```cpp
#include <iiSocietySync.h>

// transport는 이미 계정 증명과 TLS 확인을 마친 Society 전송 객체이다.
iiSocietySync::Controller sync([&](const QString &peer, const QJsonObject &request) {
    return transport.request(peer, request);
});
sync.open(containerRoot, verifiedAccountScope); // 64자리 소문자 SHA-256 scope
sync.setPeers(authorizedPeerIds, remoteHostIds);
// transport의 요청 핸들러: sync.handle(authenticatedPeerId, envelope)
// transport의 완료 콜백: sync.receive(transportRequestId, response)
// 로그아웃·컨테이너 변경·모바일 백그라운드: sync.close()
```

`Controller`와 그 전송 콜백은 앱 스레드에서 사용한다. `open()`은 비동기이며 `changed()` 후 `available()`로 결과를 확인한다. 파일 해시·SQL·실제 적용은 전용 스레드에서 수행한다. `synchronized(peer)`는 해당 회차의 양방향 적용과 커서 저장이 완료되었다는 뜻이다. 원격 호스트 목록은 중복 제거·정렬하여 순차 처리하고, 연결 중 5초 간격으로 변경을 검사한다. 모바일 Society는 호스트 리스너를 만들지 않고 클라이언트 요청으로 내려받기와 올리기를 모두 수행한다.

`Replica::handle()`은 인증이 끝난 내부 작업 API이다. 네트워크에 직접 노출하면 안 된다. `Controller::handle()`은 현재 인증된 peer 집합·scope·프로토콜·요청 크기를 검사한다. 공개 scope를 아는 것만으로는 접근 권한이 생기지 않는다. SDK는 비밀번호·쿠키·일별 seed를 받거나 iisacc.com에 HTTP 요청을 보내지 않는다. 앱의 계정 전환·로그아웃은 작업 세대 번호를 바꾸어 이전 응답과 진행 중 파일 작업을 취소한다.

## 호스트의 동일 드라이브를 미러링하는 절차

프로토콜 2의 `describe` 응답으로 인증된 호스트의 논리 UUID와 기기 replica UUID를 확인한다. 이전의 독립 드라이브 병합 프로토콜 1은 거부한다. 첫 연결은 `bindHost`로 호스트를 고정하고 기존 클라이언트 8개 영역의 내용을 `.society-sync/detached/<old-id>-<migration-id>/`로 보존한다. 이동 계획을 SQLite에 먼저 커밋하고 영역 루트를 유지하므로 UUID 변경이나 일부 이동 직후 중단돼도 `open()`에서 복구한다.

`SocietyDrive::adoptReplicaIdentity`가 호스트 UUID를 채택하고 native 공개 상태를 `replicaReady: false`로 전환한다. 전체 호스트 상태와 전송 중 추가 변경을 내려받아 따라잡은 뒤 `completeReplica`로 공개하고 양방향 동기화를 시작한다. 이전 클라이언트 전용 파일은 업로드하지 않는다. 기존 파일을 새 드라이브에 추가하려면 보존된 복구 영역에서 명시적으로 가져온다. 초기 미러 완료 후의 오프라인 편집·생성·삭제는 다음 연결에 호스트를 거쳐 다른 클라이언트로 전파된다.

`binding(path)`은 로컬 미러의 primary, 논리 UUID, 완료 상태, 복구 경로를 읽는다. `mirrorChanged`는 제품이 드라이브 객체와 화면을 다시 여는 이벤트이다. `claimPrimaryHost`/`primaryHost`의 기기 로컬 소유 기록은 새로운 더 작은 ID의 데스크톱이 기존 호스트를 대체하지 않게 한다. 미러는 자동으로 호스트가 되지 않는다.

## 저장과 전송

8개 영역의 상대 경로를 동기화한다. 일반 파일·디렉터리와 삭제 기록이 대상이다. 루트의 `.society-drive.json`, `.society-sync/`, 로그인·페어링 그룹 상태, Helper 메시지는 전송하지 않는다. 호스트와 모든 클라이언트의 논리 컨테이너 UUID는 동일하다. replica UUID와 OS 제공자의 내부 등록 ID는 기기마다 유지한다. Finder·Files·공개 `filesHandler`는 계속 `Files/`만 노출한다.

`.society-sync/journal.sqlite`는 기기 로컬 메타데이터이며 계정 scope·컨테이너 UUID에 묶인다. 파일별 SHA-256, 벡터 시계, 버전, 증가하는 커서, 삭제 tombstone, 상대 기기의 replica/container ID와 양방향 커서를 저장한다. 실제 파일의 장치·파일 ID·크기·수정/변경 시각이 그대로이면 해시를 다시 계산하지 않는다. 변경 목록은 최대 128항목·약 256 KiB씩 고정된 상한 커서까지 읽는다. 그 이후 변경은 다음 회차에 전송한다. 변경이 없는 회차는 파일 바이트를 보내지 않는다.

파일은 256 KiB 청크를 `.society-sync/transfers/`에 이어 쓴다. 동일 청크 재전송은 중복 적용하지 않는다. 완료 크기와 SHA-256을 확인한 후 디렉터리 상대 원자적 rename으로 설치한다. 손상된 완성 임시 파일은 제거하여 다음 시도에서 다시 받고 기존 목적지를 유지한다. 실패·연결 종료·재실행 후에도 이미 확인된 바이트부터 재개한다. 진행 중 원본이 바뀌면 해당 전송을 실패시키고 다음 회차에서 새 버전을 처리한다.

벡터 시계로 인과관계와 동시 편집을 구분한다. 인과적으로 새 버전은 반영하고, 동시에 다른 내용으로 편집된 파일은 결정적인 우선순위로 원본 하나를 정하되 다른 내용도 `.sync-conflict-<version>` 파일로 보존한다. 파일과 삭제의 동시 충돌에서는 파일을 보존한다. 디렉터리와 파일의 충돌에서는 디렉터리를 보존하고 파일 내용을 충돌 사본으로 남긴다. 파일명 길이와 기존 충돌 이름도 검사한다. 대소문자·Unicode 정규화로 같은 파일을 가리키는 서로 다른 이름은 덮어쓰지 않고 오류를 반환한다.

일반 파일을 대체·삭제하기 전에 `.society-sync/recovery/<UUID>`에 기존 파일의 hard link와 경로·시각 JSON을 남긴다. 복구 자료는 다른 기기에 전송하지 않으며 자동 삭제하지 않는다. 별도 스냅샷/백업 제품은 아니므로 다른 프로세스가 기존 열린 inode를 계속 수정하면 복구 hard link에도 반영될 수 있다.

## 경계와 운용 제약

- macOS/iOS/Android/Linux는 POSIX 파일 접근을, Windows는 네이티브 NT 파일 핸들을 사용한다. Windows의 로컬 볼륨은 파일 ID와 hard link를 지원해야 한다. 열기 시 비공개 영역에서 실제 복구 링크 생성·읽기·정리를 검사하므로 지원 플래그만으로 판단하지 않는다. UNC/네트워크 드라이브, FAT처럼 복구 hard link를 지원하지 않는 볼륨은 거부한다. Windows 교차 빌드와 실제 Windows/Wine 실행 결과는 별도로 기록한다.
- 기기 로컬 디스크만 사용한다. 알려진 SMB/NFS 등의 네트워크 파일 시스템, 심볼릭 링크·특수 파일, 컨테이너 교체·저널 우회 경로를 거부한다. POSIX는 `openat`/`O_NOFOLLOW`와 루트 inode 확인으로 경로를 제한한다. Windows는 [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)의 `RootDirectory` 상대 열기, reparse point 거부, 볼륨·128비트 파일 ID 확인을 사용한다. 작업 동안 모든 상위 디렉터리 핸들을 유지해 경로 교체를 막는다. 읽기·해시 중인 파일의 동시 쓰기를 거부하며, 공유 위반은 다음 동기화에서 재시도한다.
- Windows에서 예약 장치 이름, ADS 구분자, 끝의 점·공백, Win32에서 표현할 수 없는 이름은 변경 없이 거부한다. 이름을 자동 정규화하거나 다른 이름으로 바꾸지 않는다.
- 구성 요소 이름은 UTF-8 220바이트 이하, 경로는 3,500 UTF-16 코드 단위 이하이다. OS의 더 짧은 경로 제한도 적용된다. 한 컨테이너 검사에는 최대 250,000개 항목, 한 벡터 시계에는 최대 64개 replica를 허용한다.
- 최초 인증된 primary host ID를 고정한다. 같은 호스트가 다른 드라이브를 선택하면 기존 미러와 미전송 변경을 복구 영역에 보존하고 새 초기 미러를 시작한다. 다른 primary로의 임의 자동 전환은 거부한다. 같은 논리 호스트의 저널 복구는 커서를 재설정하여 재검사한다.
- 삭제 기록과 복구 자료는 자동 GC하지 않는다. 영구 오프라인 기기의 복귀 가능성을 유지하며 디스크 사용량은 누적된다. 저장 공간 부족 시 전송을 멈춘다.
- Windows 청크 데이터는 write-through와 `FlushFileBuffers`로 기록하고, 교체·hard link·복구 이동은 열린 디렉터리 상대 NT 작업으로 수행한다. 프로세스 중단 복구와 전원 차단 내구성은 구분하며 Windows의 전원 차단 시험은 별도이다.
- 원자성은 파일 단위이다. 여러 파일이나 사용 중인 데이터베이스를 하나의 트랜잭션으로 스냅샷하지 않는다. 소비 앱은 데이터베이스 백업/닫기·일관된 파일 내보내기를 통해 동기화 가능한 완료 상태를 저장해야 한다.
- `Controller`의 요청은 120초까지 기다리고 pending 응답은 10~500ms 간격으로 재확인한다. 해시는 UI 스레드를 막지 않으며 취소를 각 블록 사이에 확인한다. 종료 후에는 전송 객체의 콜백도 해제해야 한다.

## 빌드·설치·검증

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos;$HOME/.local/SDK"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
cmake -S tests/consumer -B build/consumer \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos;$HOME/.local/SDK" \
  -DiiSocietySync_DIR="$HOME/.local/SDK/iiSocietySync/lib/cmake/iiSocietySync"
cmake --build build/consumer --parallel 4
ctest --test-dir build/consumer --output-on-failure
```

공개 헤더는 설치 시 `include/`에 놓이며 소스 헤더는 구현과 같은 디렉터리에 둔다. iOS는 정적 라이브러리, 데스크톱·Android는 공유 라이브러리이다. 각 ABI의 iiSocietyContainer 0.10.0·iiServerHost 0.4.1을 먼저 설치한다. Society의 `tools/build_ios.py`와 `tools/build_android.py`는 이 순서를 포함한다.

파일 접근 계약 검사는 청크 재전송, 해시·부분 읽기, 원본 보존, 실제 심볼릭 링크 우회 거부, 복구 이동 재시도와 루트 교체를 검사한다. Windows는 `CreateSymbolicLinkW`로 reparse fixture를 생성하므로 Developer Mode 또는 심볼릭 링크 생성 권한이 필요하다. `QFile::link`의 Windows shortcut은 대체 검증으로 사용하지 않는다. 교차 실행 시 `IISOCIETYSYNC_TEST_DIRECTORY`를 대상 런타임에서 보이는 `build/` 경로로 지정한다.

검사는 8개 영역·비공개 루트 제외, 페이지 커서, 실제 양방향 파일·디렉터리·삭제, 새 기기의 과거 삭제 이력 수신, 동시 수정·삭제·유형 충돌, 중단·ACK 유실·재실행, 해시 오류·경로 우회·파일명 충돌, 인증 철회·토큰 재사용 거부, 실제 loopback TLS의 모바일 클라이언트 역할을 다룬다. 설치 소비자는 소스 트리의 헤더·라이브러리를 참조하지 않고 같은 기능 검사를 실행하며 Helper 의존이 없는지도 확인한다. 물리 기기 간 Wi-Fi·OS 백그라운드 검증은 이 검사들과 구분한다.

`./install.sh`는 같은 구성·빌드·검사·설치와 `build/consumer/build/`의 설치 소비자 검사를 수행한다. `QT_PREFIX_PATH`, `INSTALL_PREFIX`, `CMAKE_PREFIX_PATH`로 경로를 바꿀 수 있고 기본 SDK 검색 경로에는 `$HOME/.local/SDK`를 포함한다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiSocietySync의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용이다. 전체 조건은 [LICENSE](LICENSE)를 따른다. Qt와 다른 외부 구성 요소의 라이선스는 각각 유지한다.
