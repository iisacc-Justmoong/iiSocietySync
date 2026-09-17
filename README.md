# iiSocietySync 0.8.0

## 메타데이터 우선과 선택 다운로드

호스트의 최초 색인은 복제 요청 처리와 별도 스레드·SQLite 연결에서 수행한다. 디렉터리 목록을 검사한 뒤 작은 파일과 썸네일부터 SHA-256을 확정하며, 최대 64개 항목 또는 약 50ms마다 짧은 트랜잭션으로 저장한다. 1MiB보다 큰 파일을 읽기 전에는 앞선 묶음을 확정한다. 파일 해시를 계산하는 동안 복제 작업 잠금을 보유하지 않으며 `Controller`의 `changes` 응답은 이미 확정한 목록을 제공한다. 새 대형 모델의 해시가 끝나기 전에도 작은 파일의 목록과 전송을 처리할 수 있다.

색인이 취소되거나 다음 파일이 실패해도 앞서 확정한 해시와 리비전은 유지한다. 재시작 시 파일 식별자·크기·시각이 같은 항목은 다시 읽지 않는다. 삭제 판정은 전체 탐색과 파일 검사가 끝난 뒤 현재 경로의 부재를 다시 확인하고 수행한다. 해시 이후에도 파일 식별 상태를 재검사하여 동시 수정의 오래된 해시를 확정하지 않는다. `iiSocietySync.Indexing`은 대형 해시 중 메타데이터 응답, 작은 항목의 선행 확정, 취소 후 재사용, 동시 파일 재생성의 거짓 삭제 방지를 검사한다.

직접 사용하는 `Replica::handle(peer, request)`는 기존 동기식 색인 동작을 유지한다. 별도 색인 관리자가 있는 소비자는 세 번째 인자 `refreshIndex=false`로 확정된 목록만 조회할 수 있다. 별도 SQLite 연결을 여는 작업은 짧은 색인 트랜잭션과 경합할 때 최대 2초를 기다린다. 호스트 종료·컨테이너 교체는 색인 스레드를 취소하고 정리하며, 비동기 앱 종료는 호출 스레드를 기다리게 하지 않는다. 개별 대형 파일의 최초 SHA-256 계산 자체와 파일시스템 I/O 시간은 여전히 필요하다.

유휴 호스트는 전체 트리를 매초 다시 색인하지 않는다. 파일 감시 이벤트는 기존 75ms debounce 후 색인을 요청하고, 수동 동기화도 즉시 요청한다. 감시는 최대 8,192개이며 POSIX에서는 프로세스 파일 핸들 소프트 한도의 1/8, iOS·Android에서는 추가로 최대 128개로 제한한다. kqueue 감시가 TLS·SQLite·원본 파일에 필요한 핸들을 소진하지 않도록 한다. 이 한도 밖의 변경이나 놓친 이벤트는 30초 주기의 전체 탐색으로 보완한다. 복제 요청·클라이언트 동기화 타이머는 계속 1초 간격으로 동작한다. 이미 진행 중인 색인 도중 새 로컬 파일이 생기면 다음 색인 회차에서 확인하며, 수신된 push는 색인 완료를 기다리지 않고 자체 검증·리비전 확정 경로를 사용한다.

`SOCIETY_SYNC_TRACE=1`은 초기화 때 적용한 감시 수 한도만 진단 로그에 표시한다. `Indexing` 검사는 파일 핸들 한도를 256으로 낮춘 프로세스에서 300개 파일을 열거한 뒤 파일 읽기·TCP 소켓 열기·수동 재색인이 계속 가능한지 확인한다. macOS와 iOS의 감시 백엔드 차이로 실기기 kqueue 오류의 재현·소멸은 별도 기기 로그로 검증한다.

`Controller`는 기본 `MetadataFirst` 정책으로 호스트의 리비전·목록을 먼저 복제한다. `Synchronizer`를 직접 사용하는 기존 소비자는 기본 `FullReplica`를 유지하며 `setContentPolicy`로 선택한다. `StorageMap`의 원본 미보유 항목은 로컬 파일 삭제로 해석하지 않는다. 확인된 캐시에 호스트 변경이 도착하면 이전 바이트를 무효화하고, 아직 승인되지 않은 로컬 수정은 기존 충돌 보존 경로로 처리한다.

`StorageMap::request(keys)`는 선택한 버전을 고정한다. 완료 후 SHA-256·크기를 검증하고 원자적으로 공개하며, 호스트 버전 변경은 요청을 실패시켜 재선택하게 한다. 취소는 진행 중 청크 묶음 이후 추가 다운로드를 멈추고 미완성 원본을 공개하지 않는다. 부분 파일은 다음 명시 요청에서 이어받는다. 로컬에서 완성한 파일은 동일 프로토콜로 호스트에 제출하고 확정 리비전을 받는다.

클라이언트에 과거 미확정 수정이 많이 남아 있어도 원격 전용 파일의 메타데이터를 먼저 적용·게시한다. 디렉터리·삭제의 구조 순서를 유지한 뒤 메타데이터, 보유한 바이트에 대한 호스트 확정, 명시적으로 선택한 원본, 나머지 식별 자료·충돌 조정 순서로 처리한다. 선택 원본은 자기 파일의 검증·원자적 커밋이 끝나면 즉시 요청을 완료하므로 다른 파일의 업로드·충돌 조정을 기다리지 않는다. 기존 로컬 수정은 계속 충돌 보존 경로를 사용하며 삭제하거나 강제로 승인하지 않는다.

호스트 저널에 아직 없는 새 로컬 파일은 선택 다운로드 이후, 무관한 기존 충돌 파일의 원본 조정 이전에 제출한다. 전체 제출 매니페스트 검증과 부모 디렉터리 순서를 유지하며 새 파일 중 최근 로컬 변경을 우선한다. 이때 미처리 항목을 넘겨 커서를 진행하지 않는다. 업로드 뒤 호스트 저널을 다시 받아 해당 파일의 확정 리비전을 먼저 반영한다. `OnDemand` 회귀 검사는 무관한 충돌 파일의 응답을 멈춘 상태에서도 목록 게시, 선택 다운로드 완료, 새 파일 업로드 및 호스트 확정이 진행되는지 검사한다.

설치 소비자의 `installed_on_demand`도 같은 우선순위·취소·선택 원본 검사들을 배포된 SDK에 연결해 실행한다.

비동기 색인 회귀에서는 첫 namespace 변경 신호를 모든 파일의 색인 완료로 간주하지 않는다. 오프라인 파일 게시와 느린 공급자 중 동시 로컬 커밋은 검사 대상 경로가 실제 저장소 맵에 나타나는 것을 확인한다.

사진 alias·프리뷰(각 512 KiB 이하)와 모델 `model_index.json`(1 MiB 이하)은 식별 자료로 먼저 받는다. 일반 이미지 프리뷰는 호스트에서 256 px JPEG로 생성한다. 디코딩 입력은 32 MiB·64 MP, 응답은 512 KiB로 제한하며 미지원 프리뷰가 원본 목록을 숨기지 않는다. 원본은 선택 요청에만 전송한다. 공급자 I/O 후 저널 재진입은 최대 2초 동안 잠금을 기다려 목록 게시와 일시적으로 경합해도 이미 성공한 저장을 실패로 처리하지 않는다.

프리뷰는 회차당 최대 16개씩 처리한다. 큰 갤러리의 프리뷰를 모두 받기 전에 다음 회차에서 새 모델 요청과 생성 결과 업로드를 처리한다. `iiSocietySync.OnDemand`는 이 처리 순서와 원본 전송 없는 초기 목록·프리뷰, 선택 모델만 다운로드, 생성 결과 업로드, 재시작 후 거짓 삭제 방지, 변경된 버전 거부, 전송 도중 취소와 이어받기를 검사한다.

`BleDiscovery`/`NearbyBootstrap`이 근처 기기와 작은 연결 정보를 교환한다. 인증된 Wi-Fi/LAN 파일 전송은 최대 4개 조각을 동시에 처리하며 iiServerHost 0.6의 바이너리 본문 협상을 사용한다. [연결 흐름, API, 플랫폼 지원과 검증 범위](NearbySync.md)를 참고한다. 호스트 권한·확정 리비전·공급자 분리는 [Namespace 계약](Namespace.md)을 따른다. 재개·SHA-256 검증은 유지한다.

`iiSocietyContainer` 0.14.0 이상을 사용한다. `Files/Documents`, `Files/Audios`, `Files/3D objects`는 삭제할 수 없는 기본 디렉터리이다. 최상위 `Photos/`는 `photos` 섹션 키로 사진 객체·프리뷰를 동기화한다. 원격 삭제가 도착해도 빈 기본 폴더를 유지하며 더 높은 로컬 버전으로 기록하여 반복 전송을 처리한다. 해당 경로를 파일로 교체하는 요청은 충돌 사본으로 보존한다. 폴더 내부 항목의 생성·수정·삭제는 기존 동기화 규칙을 따른다. 최초 호스트 채택 시 기본 폴더 내부의 이전 데이터만 복구 영역으로 보관하고 폴더 자체는 유지한다. `Replica` 테스트는 삭제·파일 교체·재전송·자식 삭제·호스트 채택을 검사한다.

서로 다른 기기에서 실행 중인 Society의 컨테이너 데이터를 동기화하는 C++23/Qt SDK이다. 같은 계정의 인증된 Society 연결을 받아 변경 감지, 양방향 전송, 중단 복구, 충돌 보존을 수행한다. `helloWorld()`는 기존 소비자 호환용으로 유지한다.

## 책임과 의존 방향

| 구성 요소 | 담당하는 일 | 의존 경계 |
| --- | --- | --- |
| iiSocietyHelper | 같은 기기의 앱 관측, 메시지·ACK, 객체 스냅샷, 계정 모델 참조, 로컬 공통 파일 접근 | Container·Account를 사용하며 Sync·ServerHost에 의존하지 않는다. |
| iiSocietySync | 다른 기기의 Society 컨테이너 복제, 원격 Files 탐색·다운로드 | Container·ServerHost를 사용하며 Helper·제품 앱에 의존하지 않는다. |
| iiSocietyContainer | 컨테이너 UUID, 9개 영역, 로컬 공유·OS 제공자 | 동기화 프로토콜을 알지 않는다. |
| iiServerHost | 인증된 일반 요청·응답, LAN TLS·기존 relay, 제한된 파일 서비스 | Society 변경 기록·충돌 정책을 알지 않는다. |
| iiSocietyClient | 공유 로그인·그룹 상태, 계정 증명, 발견·자동 페어링, 소비 앱의 호스트 연결 | Sync·Account를 사용하며 Product 앱에 의존하지 않는다. |
| Society 앱 | 최초 로그인·호스트 선택, 앱 수명·화면 | Client의 공유 인증 구성과 Sync를 연결하고 호스트 역할을 관리한다. |

기존 Qt 6.8.3 Core/Gui/Network/Sql, SQLite와 iiServerHost 0.6.0의 TLS를 재사용한다. 새 외부 라이브러리·서버·유료 서비스를 추가하지 않는다. 해시에는 Qt의 [QCryptographicHash](https://doc.qt.io/qt-6.8/qcryptographichash.html), 프로세스 간 작업 배제에는 [QLockFile](https://doc.qt.io/qt-6.8/qlockfile.html), 영속 저널에는 [SQLite WAL](https://sqlite.org/wal.html)을 사용한다. 유지보수·라이선스는 기존 Qt/SQLite/SDK 구성의 조건을 따른다. Society 고유의 버전·충돌 정책만 이 SDK에서 구현한다.

## 공개 API

- `Controller`: 전용 작업 스레드, 컨테이너 열기·닫기, 인증된 기기 집합, 순차 동기화, 원격 요청 처리.
- `Replica`: 단일 스레드의 Namespace 권한·리비전 그래프·SQLite 변경 저널과 로컬 투영. `scan`, `changes`, `record`, `handle`, 기기별 커서 영속화.
- `ObjectProvider`: 호스트의 내용 해시 기반 바이트 배치·복구. `DirectoryObjectProvider`와 `RemoteObjectProvider`를 `Controller::placeObject`/`restoreObject`로 실행한다.
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
// 로그아웃·컨테이너 변경·OS 백그라운드 실행 시간 만료: sync.close()
```

`Controller`와 그 전송 콜백은 앱 스레드에서 사용한다. `open()`은 비동기이며 `changed()` 후 `available()`로 결과를 확인한다. 파일 해시·SQL·실제 적용은 전용 스레드에서 수행한다. `synchronized(peer)`는 해당 회차에 제출한 변경의 호스트 확정 결과를 다시 적용하고 커서를 저장했다는 뜻이며, 호스트의 아직 진행 중인 최초 색인 전체가 끝났다는 뜻은 아니다. 연결된 peer가 없어도 로컬 변경을 기록한다. 원격 호스트 목록은 중복 제거·정렬하여 순차 처리하고, 로컬 파일 변경은 QFileSystemWatcher로 감지해 75ms 동안 합친 뒤 즉시 동기화를 예약한다. 진행 중 들어온 변경은 완료 직후 다시 처리한다. 복제와 클라이언트 재검사는 1초 주기이며, 유휴 호스트의 누락 이벤트·감시 한도 초과 경로는 30초 주기의 전체 색인이 보완한다. OS 감시 경로는 최대 8,192개이다. 모바일 Society는 호스트 리스너를 만들지 않고 클라이언트 요청으로 내려받기와 올리기를 모두 수행한다.

`Replica::handle()`은 인증이 끝난 내부 작업 API이다. 네트워크에 직접 노출하면 안 된다. `Controller::handle()`은 현재 인증된 peer 집합·scope·프로토콜·요청 크기를 검사한다. 공개 scope를 아는 것만으로는 접근 권한이 생기지 않는다. SDK는 비밀번호·쿠키·일별 seed를 받거나 iisacc.com에 HTTP 요청을 보내지 않는다. 앱의 계정 전환·로그아웃은 작업 세대 번호를 바꾸어 이전 응답과 진행 중 파일 작업을 취소한다.

## 호스트의 동일 드라이브를 미러링하는 절차

프로토콜 2 + `namespaceVersion: 1`의 `describe` 응답으로 인증된 호스트의 논리 UUID와 기기 replica UUID를 확인한다. 이전의 독립 드라이브 병합 프로토콜 1은 거부한다. 첫 연결은 `bindHost`로 호스트를 고정하고 기존 클라이언트 9개 영역의 내용을 `.society-sync/detached/<old-id>-<migration-id>/`로 보존한다. 이동 계획을 SQLite에 먼저 커밋하고 영역 루트를 유지하므로 UUID 변경이나 일부 이동 직후 중단돼도 `open()`에서 복구한다.

`SocietyDrive::adoptReplicaIdentity`가 호스트 UUID를 채택하고 native 공개 상태를 `replicaReady: false`로 전환한다. 호스트 메타데이터와 전송 중 추가 변경을 내려받아 따라잡은 뒤 `completeReplica`로 공개하고 양방향 동기화를 시작한다. 이전 클라이언트 전용 파일은 업로드하지 않는다. 기존 파일을 새 드라이브에 추가하려면 보존된 복구 영역에서 명시적으로 가져온다. 초기 미러 완료 후의 오프라인 편집·생성·삭제는 다음 연결에 호스트를 거쳐 다른 클라이언트로 전파된다.

`binding(path)`은 로컬 미러의 primary, 논리 UUID, 완료 상태, 복구 경로를 읽는다. `mirrorChanged`는 제품이 드라이브 객체와 화면을 다시 여는 이벤트이다. `claimPrimaryHost`/`primaryHost`의 기기 로컬 소유 기록은 새로운 더 작은 ID의 데스크톱이 기존 호스트를 대체하지 않게 한다. 미러는 자동으로 호스트가 되지 않는다.

## 저장과 전송

9개 영역의 상대 경로를 동기화한다. 일반 파일·디렉터리와 삭제 기록이 대상이다. 루트의 `.society-drive.json`, `.society-sync/`, 로그인·페어링 그룹 상태, Helper 메시지는 전송하지 않는다. 호스트와 모든 클라이언트의 논리 컨테이너 UUID는 동일하다. replica UUID와 OS 제공자의 내부 등록 ID는 기기마다 유지한다. Finder·Files·공개 `filesHandler`는 계속 `Files/`만 노출한다.

iiSocietyContainer가 새 컨테이너의 `Models/`에 만드는 유형별 빈 폴더도 일반 디렉터리 항목으로 동기화한다. 따라서 파일 변경 수와 페이지 경계 테스트는 컨테이너 초기 스캔의 커서 이후를 비교한다. 초기 폴더의 매니페스트 형식도 검사하며, 첫 미러 복구 테스트는 이전 파일의 비공개 보존과 호스트의 빈 모델 폴더 구조 복원을 함께 확인한다.

`.society-sync/journal.sqlite`는 기기 로컬 메타데이터이며 계정 scope·컨테이너 UUID에 묶인다. 파일별 SHA-256, 벡터 시계, 버전, 증가하는 커서, 삭제 tombstone, 상대 기기의 replica/container ID와 양방향 커서를 저장한다. 실제 파일의 장치·파일 ID·크기·수정/변경 시각이 그대로이면 해시를 다시 계산하지 않는다. 변경 목록은 최대 128항목·약 256 KiB씩 고정된 상한 커서까지 읽는다. 그 이후 변경은 다음 회차에 전송한다. 변경이 없는 회차는 파일 바이트를 보내지 않는다.

`describe.manifestVersion = 1`인 상대와는 양방향 식별 정보 교환을 파일 바이트보다 먼저 끝낸다. 호스트의 `changes` 페이지를 모두 수집하고, 클라이언트도 `manifest` 페이지로 상대 경로·종류·크기·SHA-256·벡터 시계·버전·순번을 먼저 보낸다. 각 매니페스트는 최대 250,000개 항목이며, 최대 128항목·256 KiB 단위로 전송한다. 전체 식별자는 각 항목의 compact JSON과 개행을 순서대로 SHA-256에 넣은 값이다. 수신자는 페이지 순서·커서·복제본 ID·전체 해시를 검증하고 마지막 페이지를 ACK한다. ACK가 어긋나면 파일 바이트를 전송하지 않는다. 매니페스트 수신은 파일·임시 청크를 변경하지 않으며, 클라이언트는 파일 적용 전에 별도 저널 페이지에서 호스트 리비전을 검증·저장한다. 첫 미러의 독립 파일은 빈 매니페스트로 제외하고, 내려받기 중 새로 생긴 변경·충돌 사본은 업로드 전에 다시 모두 공고한다. 전송 요청은 확인된 매니페스트 ID에 묶는다. Namespace 지원을 광고하지 않는 구버전 호스트는 연결을 거부하고 업데이트 오류를 반환한다.

대형 파일은 약 1초의 payload 처리 후 청크 경계에서 매니페스트를 다시 확인한다. 디렉터리 준비·깊은 경로부터의 삭제·작은 파일 순으로 처리하므로 새 작은 변경이 대형 파일 전체 완료를 기다리지 않는다. 부분 파일과 미완료 커서는 유지하고 `begin.offset`에서 이어받아 완료 청크를 다시 보내지 않는다. 매니페스트 ACK는 갱신할 때마다 다시 검증한다. 이 시간은 예약 기준이며 한 요청의 지연·해시·파일 적용 시간을 포함한 완료 상한은 아니다. 내려받기와 올리기 중 새 수정/삭제 우선 반영 및 청크 오프셋의 연속성을 지연 응답 회귀로 검증한다.

파일은 256 KiB 청크를 `.society-sync/transfers/`에 이어 쓴다. 동일 청크 재전송은 중복 적용하지 않는다. 완료 크기와 SHA-256을 확인한 후 디렉터리 상대 원자적 rename으로 설치한다. 손상된 완성 임시 파일은 제거하여 다음 시도에서 다시 받고 기존 목적지를 유지한다. 실패·연결 종료·재실행 후에도 이미 확인된 바이트부터 재개한다. 진행 중 원본이 바뀌면 해당 전송을 실패시키고 다음 회차에서 새 버전을 처리한다.

벡터 시계는 제출의 인과관계 추적에 사용하며 확정 순서는 호스트의 리비전 저널이 정한다. 클라이언트는 기준 리비전을 제출하고 호스트의 결정을 받는다. 오래된 기준의 파일 편집은 호스트 상태를 유지하고 다른 내용을 `.sync-conflict-<version>`으로 보존한다. 구조 충돌에서는 자식 파일을 잃지 않도록 디렉터리를 유지하고 파일을 충돌 사본으로 남긴다. 파일명 길이와 기존 충돌 이름을 검사하며 대소문자·Unicode 정규화 충돌은 오류로 반환한다. [상세 결정 규칙](Namespace.md)을 따른다.

일반 파일을 대체·삭제하기 전에 `.society-sync/recovery/<UUID>`에 기존 파일의 hard link와 경로·시각 JSON을 남긴다. 복구 자료는 다른 기기에 전송하지 않으며 자동 삭제하지 않는다. 별도 스냅샷/백업 제품은 아니므로 다른 프로세스가 기존 열린 inode를 계속 수정하면 복구 hard link에도 반영될 수 있다.

## 경계와 운용 제약

- macOS/iOS/Android/Linux는 POSIX 파일 접근을, Windows는 네이티브 NT 파일 핸들을 사용한다. Windows의 로컬 볼륨은 파일 ID와 hard link를 지원해야 한다. 열기 시 비공개 영역에서 실제 복구 링크 생성·읽기·정리를 검사하므로 지원 플래그만으로 판단하지 않는다. UNC/네트워크 드라이브, FAT처럼 복구 hard link를 지원하지 않는 볼륨은 거부한다. Windows 교차 빌드와 실제 Windows/Wine 실행 결과는 별도로 기록한다.
- 저널과 소비 앱용 파일 투영은 기기 로컬 디스크에 둔다. NAS/S3의 객체 저장은 ObjectProvider를 사용한다. 알려진 SMB/NFS 등의 네트워크 파일 시스템, 심볼릭 링크·특수 파일, 컨테이너 교체·저널 우회 경로를 거부한다. POSIX는 `openat`/`O_NOFOLLOW`와 루트 inode 확인으로 경로를 제한한다. Windows는 [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)의 `RootDirectory` 상대 열기, reparse point 거부, 볼륨·128비트 파일 ID 확인을 사용한다. 작업 동안 모든 상위 디렉터리 핸들을 유지해 경로 교체를 막는다. 읽기·해시 중인 파일의 동시 쓰기를 거부하며, 공유 위반은 다음 동기화에서 재시도한다.
- Windows에서 예약 장치 이름, ADS 구분자, 끝의 점·공백, Win32에서 표현할 수 없는 이름은 변경 없이 거부한다. 이름을 자동 정규화하거나 다른 이름으로 바꾸지 않는다.
- 구성 요소 이름은 UTF-8 220바이트 이하, 경로는 3,500 UTF-16 코드 단위 이하이다. OS의 더 짧은 경로 제한도 적용된다. 한 컨테이너 검사에는 최대 250,000개 항목, 한 벡터 시계에는 최대 64개 replica를 허용한다.
- 최초 인증된 primary host ID를 고정한다. 같은 호스트가 다른 드라이브를 선택하면 기존 미러와 미전송 변경을 복구 영역에 보존하고 새 초기 미러를 시작한다. 다른 primary로의 임의 자동 전환은 거부한다. 같은 논리 호스트의 저널 복구는 커서를 재설정하여 재검사한다.
- 삭제 기록과 복구 자료는 자동 GC하지 않는다. 영구 오프라인 기기의 복귀 가능성을 유지하며 디스크 사용량은 누적된다. 저장 공간 부족 시 전송을 멈춘다.
- Windows 청크 데이터는 write-through와 `FlushFileBuffers`로 기록하고, 교체·hard link·복구 이동은 열린 디렉터리 상대 NT 작업으로 수행한다. 프로세스 중단 복구와 전원 차단 내구성은 구분하며 Windows의 전원 차단 시험은 별도이다.
- 원자성은 파일 단위이다. 여러 파일이나 사용 중인 데이터베이스를 하나의 트랜잭션으로 스냅샷하지 않는다. 소비 앱은 데이터베이스 백업/닫기·일관된 파일 내보내기를 통해 동기화 가능한 완료 상태를 저장해야 한다.
- `Controller`의 요청은 120초까지 기다리고 pending 응답은 1~100ms 간격으로 재확인한다. 해시는 UI 스레드를 막지 않으며 취소를 각 블록 사이에 확인한다. 종료 후에는 전송 객체의 콜백도 해제해야 한다.

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

공개 헤더는 설치 시 `include/`에 놓이며 소스 헤더는 구현과 같은 디렉터리에 둔다. iOS는 정적 라이브러리, 데스크톱·Android는 공유 라이브러리이다. 각 ABI의 iiSocietyContainer 0.13.0·iiServerHost 0.6.0을 먼저 설치한다. Society의 `tools/build_ios.py`와 `tools/build_android.py`는 이 순서를 포함한다.

파일 접근 계약 검사는 청크 재전송, 해시·부분 읽기, 원본 보존, 실제 심볼릭 링크 우회 거부, 복구 이동 재시도와 루트 교체를 검사한다. Windows는 `CreateSymbolicLinkW`로 reparse fixture를 생성하므로 Developer Mode 또는 심볼릭 링크 생성 권한이 필요하다. `QFile::link`의 Windows shortcut은 대체 검증으로 사용하지 않는다. 교차 실행 시 `IISOCIETYSYNC_TEST_DIRECTORY`를 대상 런타임에서 보이는 `build/` 경로로 지정한다.

검사는 9개 영역·비공개 루트 제외, 페이지 커서, 실제 양방향 파일·디렉터리·삭제, 새 기기의 과거 삭제 이력 수신, 동시 수정·삭제·유형 충돌, 중단·ACK 유실·재실행, 해시 오류·경로 우회·파일명 충돌, 인증 철회·토큰 재사용 거부, 실제 loopback TLS의 모바일 클라이언트 역할을 다룬다. 설치 소비자는 소스 트리의 헤더·라이브러리를 참조하지 않고 같은 기능 검사를 실행하며 Helper 의존이 없는지도 확인한다. 물리 기기 간 Wi-Fi·OS 백그라운드 검증은 이 검사들과 구분한다.

`./install.sh`는 같은 구성·빌드·검사·설치와 `build/consumer/build/`의 설치 소비자 검사를 수행한다. `QT_PREFIX_PATH`, `INSTALL_PREFIX`, `CMAKE_PREFIX_PATH`로 경로를 바꿀 수 있고 기본 SDK 검색 경로에는 `$HOME/.local/SDK`를 포함한다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiSocietySync의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용이다. 전체 조건은 [LICENSE](LICENSE)를 따른다. Qt와 다른 외부 구성 요소의 라이선스는 각각 유지한다.

0.4.0 회귀 검사는 140개 파일의 다중 매니페스트 ACK가 첫 바이트보다 앞서는지, 잘못된 ACK에서 바이트 전송이 0회인지, 실제 TLS에서 수동 호출 없는 생성·수정·호스트 변경 전파가 1.8초 검사 한도 안에 끝나는지 확인한다. 주기와 검사 한도는 네트워크·OS가 보장하는 완료 시간이 아니다.

Apple 플랫폼의 대용량 파일 SHA-256은 OS 기본 CommonCrypto를 사용한다. 배포 Qt 6.8.3의 소프트웨어 SHA-256 병목을 줄이면서 해시 형식·파일 핸들/변경 검사·블록별 취소 계약은 유지한다. 추가 패키지나 서버 의존성 없이 macOS/iOS SDK의 시스템 라이브러리를 사용하며 다른 플랫폼은 기존 Qt 경로를 유지한다. 빈 파일·SHA 패딩 경계·1MiB 읽기 경계·바이너리 입력을 Qt의 독립 결과와 비교한다. API 근거는 [Apple CommonCrypto CommonDigest](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/CC_SHA512_Update.3cc.html) 문서이다.

데스크톱 프로세스 소유권을 반환할 때는 Controller::closeAndWait()로 이전 작업의 취소와 파일/DB 핸들 종료를 완료한 뒤 잠금을 넘긴다. 일반 close()는 기존 비동기 취소를 유지한다. Controller 검사는 실제 대용량 파일 스캔 중 반환한 즉시 다음 Replica가 잠금을 획득할 수 있는지 검사한다.


## 모바일 비동기 실행 (0.5.0)

`Controller::inspectContainer(path, scope)`는 컨테이너 UUID·미러 바인딩·primary host를 기존 복제 작업 스레드에서 읽고 `containerInspected`로 전달한다. UI에서 `Replica::binding` / `primaryHost`를 직접 호출할 필요가 없다. 조회는 데이터베이스나 복제 디렉터리를 만들지 않으며 새 조회가 들어오면 오래된 결과를 폐기한다. 신호와 `RequestSender`는 Controller 소유 스레드에서 처리하고 파일 시스템 작업은 작업 스레드에서 직렬 실행한다.

`open`의 초기 준비·SQLite·감시 등록·해시·매니페스트·청크 처리는 비동기이다. 초기 열기 실패 후 같은 경로와 계정으로 다시 `open`하여 재시도할 수 있다. 진행 중이거나 준비된 동일 컨테이너는 중복 초기화하지 않는다. `close`는 취소를 요청하고 바로 반환한다. 모바일 객체를 폐기할 때 `shutdownAsync()`를 호출하면 작업 스레드가 자원을 정리하고 스스로 종료한다. 이후 해당 Controller는 다시 열 수 없다. 데스크톱 실행권 인계에는 `closeAndWait()`를 유지한다. 기본 소멸자는 데스크톱 호환성을 위해 작업 정리를 기다린다.

`RemoteFiles`의 목적지 열기·Base64 청크 검사·쓰기·최종 `QSaveFile::commit()`도 별도 작업 스레드에서 수행한다. 저장 중에도 `busy()`를 유지하고 커밋 완료 후에만 `downloadFinished`를 전달한다. 취소와 객체 파괴는 작업을 기다리지 않으며 늦은 콜백을 무효화한다. 실패한 전송은 기존 목적지 파일을 보존한다. 이미 실행 중인 OS 파일 연산은 반환할 때 취소를 확인한다.

0.5.0에서는 RemoteFiles의 소유권 구현과 소멸 경계가 바뀌므로 소비자를 다시 빌드해야 한다. 동적 라이브러리의 ABI 이름은 `0.5`로 분리하며 기존 `0` 라이브러리를 새 구현으로 대체하지 않는다. 동기화 프로토콜 2와 디스크 형식은 유지한다. 기존 Qt Core의 [작업 객체와 queued connection](https://doc.qt.io/qt-6/qthread.html)을 재사용하며 새 외부 의존성은 없다.

`iiSocietySync.Controller`는 비동기 조회·오래된 결과 폐기·초기 실패 후 재시도·1 GiB 검사 중 비동기 종료·데스크톱 실행권 인계·다운로드 성공과 불완전 파일 보존·취소·실제 로컬 TLS 양방향 동기화를 검사한다.

Photos는 Container 0.13.0의 최상위 섹션이다. `allStoreSections()`에서 자동으로 열거하여 일반 섹션 전송·호스트 채택 규칙을 적용한다. 기존 경로 이전은 Container가 수행하며 동기화 참여 기기 모두 새 레이아웃을 사용해야 한다. Society.Photos의 TLS 통합 시험은 `Photos/`의 alias·프리뷰와 원본 채널을 함께 검사한다.

## 무결성 검사 진행 보고

`Replica::setVerificationProgress`와 `Controller::verificationProgress`는 SHA-256 처리의 실제 바이트를 전달한다. 파일별 0부터 시작하며 진행 이벤트는 처리 바이트를 의미한다. 해시 성공이나 전송 완료를 대신하지 않는다. Controller는 자기 스레드에서 최대 약 10 Hz와 시작·마지막 청크를 전달하고 이전 컨테이너의 이벤트를 폐기한다. 전송 진행 신호와 분리하여 모바일 OS의 지속 실행 작업이 큰 모델 검사 중에도 진행을 관측할 수 있게 한다. OS 실행 권한은 소비자 앱의 책임이다. ConfinedFiles 테스트는 정확한 해시·청크별 진행·중간 취소를 함께 검사한다.
