# Society Namespace와 호스트 권한

iiSocietySync 0.7은 자체 호스팅과 클라우드 호스팅에 같은 모델을 적용한다.

```text
Society Namespace (컨테이너 UUID)
  └ Logical Authority (고정한 호스트 / 저널 epoch)
      └ Revision Graph (내용 해시, 부모 리비전)
          └ Change Journal (추가 전용 순서, 이전 커밋 해시)
              └ Object Metadata (논리 경로, 종류, 크기, SHA-256, 위치)
                  ├ Mac/local storage — 인터넷 없이 읽기·편집·확정
                  ├ NAS provider
                  └ S3 provider
                          ↓
                iiSocietySync replication
                          ↓
                  다른 Society 노드
```

호스트만 확정 리비전을 만든다. 연결 경로가 BLE로 발견한 LAN/TLS인지 서버 relay인지, 바이트를 저장한 곳이 Mac·NAS·S3인지는 권한을 바꾸지 않는다. 공급자는 저장 기능이며 다른 primary host가 아니다. 기존 앱이 사용하는 로컬 Society 디렉터리는 파일 투영과 오프라인 복사본으로 유지한다.

앱이 인증된 호스트를 선택하면 `Controller::claimPrimaryHost(device)`가 작업 스레드에서 고정 정보를 저장한다. 다른 로컬 작업이 잠금을 사용 중이면 재시도하며 저장이 끝나기 전에는 복제 요청을 처리하지 않는다. 컨테이너·계정 변경은 이전 저장 요청을 취소한다. 최초 페어링과 오프라인 스캔이 경합해도 호스트 선택을 잃지 않는다.

## 리비전과 제출

- `Replica::namespaceState()`는 namespace UUID, authority epoch, 순서, head, 역할을 반환한다. epoch는 영속 host replica UUID이다. 저널을 복구하여 epoch가 달라지면 클라이언트는 저널과 커서를 초기화하고 같은 고정 호스트를 다시 따른다.
- `objectMetadata(path)`는 마지막 **호스트 확정 상태**, `record(path)`는 현재 기기의 작업 상태이다. 오프라인 편집은 `baseRevision`이 있는 제출 대기 상태이며 호스트 저널의 head를 증가시키지 않는다.
- 호스트는 제출의 기준 리비전이 현재 상태와 같을 때 변경을 확정한다. 오래된 기준의 파일 편집은 호스트 상태를 유지하고 다른 파일 내용을 `.sync-conflict-<version>`으로 보존한다. 삭제 충돌에서도 호스트의 확정 상태를 따른다. 구조 충돌은 자식 파일을 잃지 않도록 디렉터리를 유지하고 파일을 충돌 사본으로 보존하며, 이 결정 역시 호스트가 확정한다.
- 제출 응답 대기 중 다시 편집한 파일은 후속 제출로 남긴다. 앞선 제출의 확정 응답이 새 편집을 지우지 않는다.
- 회귀 검사는 호스트가 첫 제출을 저장한 뒤 ACK를 돌려주기 직전에 클라이언트를 다시 편집하여, 후속 편집과 최종 확정 리비전이 양쪽에 남는지 확인한다. TLS 파일 감시 지연 검사는 이전 회차의 확정 수신이 끝난 뒤 각 편집을 시작하며 1.8초 제한을 유지한다.
- `synchronized(peer)`는 제출 이후 호스트의 확정 리비전과 결과 파일을 다시 받은 시점이다. 단순 업로드 ACK가 아니다. 다음 연결에서 다른 클라이언트도 같은 저널을 받는다.

`NamespaceJournal`은 `namespace_revisions`에 모든 확정 메타데이터를 추가한다. 각 행에는 namespace/authority, 전역 순서, 이전 커밋 해시, 대상 객체의 부모 리비전, 내용 메타데이터, 원래 제출 버전이 있다. 수정된 대상의 최신 상태로 과거 행을 덮어쓰지 않는다. `journal(after, through)`는 이력을, `changes(after, through)`는 해당 상한 시점의 경로별 최신 상태를 페이지로 반환한다. 후속 편집이 앞서 시작한 페이지의 기록을 지우지 않는다.

클라이언트는 인증·scope 확인 이후 저널의 해시, 연속 순서, 부모, namespace/authority를 검증한다. 페이지 일부라도 손상되면 전체 페이지를 롤백한다. 파일에는 이미 확인한 호스트 리비전만 적용한다. 저널은 인증된 채널 내부의 무결성 체인이며 별도의 전자서명이나 호스트 침해 방어 장치가 아니다.

파일 전송 envelope는 기존 protocol 2에 `namespaceVersion: 1`을 필수로 추가한다. 구버전 Society는 업데이트가 필요하다. BLE discovery, 4개 조각의 TCP/TLS 전송 창, SHA-256 및 이어받기는 유지한다. scope·자격 증명·primary host 검증은 우회하지 않는다.

## 저장 공급자

`ObjectProvider`는 바이트 저장/읽기의 교체 가능한 인터페이스이다. Namespace·저널·권한 정책은 포함하지 않는다. 모든 공급자는 `(namespace UUID, SHA-256, 크기)`로 식별한 객체를 `objects/<namespace>/<hash-prefix>/<hash>`에 저장한다.

| 객체 | 저장 대상 | 동작 |
| --- | --- | --- |
| 기본 local 투영 | Society 호스트의 로컬 디렉터리 | 네트워크 없이 변경을 확정하고 모든 복제의 원본으로 사용 |
| `DirectoryObjectProvider` | 별도 Mac 디렉터리 또는 마운트한 NAS | 스트리밍 해시·크기 검사 후 원자적 저장, hard link 불필요 |
| `RemoteObjectProvider` | 명시적으로 설정한 NAS/S3 rclone remote | iiServerHost StorageBridge 사용, S3 multipart 지원은 선택한 rclone에 위임, 저장 후 다시 읽어 해시 검사 |

`Replica::placeObject(path, provider)`는 파일을 저장·검증한 다음 해당 **버전**에 공급자 위치를 기록한다. 공급자 추가나 장애는 namespace UUID와 리비전을 변경하지 않는다. 원격 I/O 동안 journal lock을 잡지 않는다. `restoreObject`는 아직 삭제 커밋이 되지 않은 누락된 로컬 투영을 복구하며, 수정된 파일과 대기 중 편집을 덮지 않는다. 이미 삭제를 확정한 파일은 보관한 ObjectIdentity로 provider `get`을 사용해 복구한 뒤 새 편집으로 확정할 수 있다.

Controller는 공급자 작업을 최대 2개의 별도 작업 스레드와 SQLite 연결로 실행한다. NAS/S3의 응답 지연 중에도 원래 호스트 작업 스레드는 로컬 확정·기기 복제를 계속한다. 계정·컨테이너 변경은 공급자 작업도 취소하고 오래된 완료 통지를 폐기하며, 데스크톱 `closeAndWait`는 공급자 작업까지 종료한 뒤 반환한다.

```cpp
// 호출 앱은 이미 사용 중인 Controller와 공급자의 수명을 관리한다.
auto nas = std::make_shared<iiSocietySync::DirectoryObjectProvider>(
    "nas-archive", mountedDedicatedDirectory, true);
sync.placeObject("models/model.safetensors", nas);
// providerFinished(path, id, restored, success, error)로 완료 확인

iiServerHost::StorageBridgeOptions backend;
backend.executable = trustedRcloneExecutable;
backend.configFile = ownerOnlyRcloneConfig;
backend.runtimeDirectory = ownerOnlyLocalRuntime;
auto s3 = std::make_shared<iiSocietySync::RemoteObjectProvider>(
    "s3-archive", "s3", "configured-s3:bucket/society", backend);
sync.placeObject("models/model.safetensors", s3);
```

공급자 API는 명시적으로 지정한 파일을 배치하는 방식이다. 전체 NAS/S3 자동 미러링 정책이나 로컬 파일을 비우는 공간 최적화 UI는 제공하지 않는다. 외부 공급자를 사용해도 소비 앱용 로컬 투영을 유지하므로 Mac Storage만으로 오프라인 사용이 가능하다. 원격 backend 실행이 없는 모바일 빌드에서는 원격 공급자가 사용 불가 오류를 반환하며 모바일은 호스트 복제 클라이언트로 동작한다.

자격 증명, endpoint와 물리 루트는 호스트의 명시적인 backend 설정에만 둔다. 복제 저널에는 넣지 않는다. 실제 NAS 마운트나 S3 계정을 자동으로 생성·설정하지 않는다. 서버 S3의 PUT/GET 일관성은 [AWS 공식 계약](https://aws.amazon.com/s3/consistency/)을 참고하되, Society 커밋 순서는 공급자와 독립적인 호스트 저널이 결정한다. 원격 복사 동작은 [rclone copyto](https://rclone.org/commands/rclone_copyto/) 계약을 사용한다.

## 이전·내구성·검증

SQLite schema 2에서 3으로 이전할 때 기존 호스트의 최신 레코드를 시작 리비전으로 가져온다. 과거에 저장하지 않았던 이력은 만들어내지 않는다. 기존 미러는 확정 저널을 처음부터 받고, 작업 파일과 오프라인 편집은 보존한다. SQLite WAL과 authority metadata는 기기 로컬 디스크에 둔다. NAS/S3에는 객체 바이트만 저장한다. 리비전 이력은 자동 GC하지 않는다.

리비전 그래프는 메타데이터 이력이다. 기본 로컬 투영이 모든 과거 파일 바이트를 보관하는 것은 아니다. 특정 버전을 provider에 배치하면 그 해시의 바이트를 별도로 보관한다. 여러 파일이나 열려 있는 앱 DB 전체를 하나의 스냅샷으로 묶지는 않는다.

`iiSocietySync.Namespace`는 오프라인 확정, 3개 노드의 호스트 결정 수렴, 메타데이터/작업 상태 분리, 이력·상한 스냅샷, 위조·재전송 롤백, schema 이전, 공급자 장애·손상·복구를 검사한다. `IISERVERHOST_TEST_RCLONE`을 설정하면 로컬의 인증된 S3 호환 서버로 실제 저장·읽기·SHA-256 왕복을 실행한다. 이는 AWS 실계정이나 실제 NAS·모바일 무선 전송 검증과 구분한다.
