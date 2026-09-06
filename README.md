# iiSocietySync

C++20과 Qt 6.8.3 Core를 사용하는 버전 0.1.0 동적 플레이스홀더 SDK이다. 공개 함수는 `Hello world!` 문자열만 반환하며, SDK 이름에 해당하는 제품 기능은 아직 구현하지 않는다. Qt 이외의 외부 의존성은 추가하지 않는다.

## 공개 API

```cpp
#include <iiSocietySync.h>

const QString message = iiSocietySync::helloWorld(); // 정확히 "Hello world!"
```

공개 헤더와 구현은 저장소 루트의 `iiSocietySync.h`, `iiSocietySync.cpp`에 함께 배치한다. `IISOCIETYSYNC_EXPORT`가 심볼을 내보내며 `IISOCIETYSYNC_BUILDING_LIBRARY`는 라이브러리 빌드에만 정의한다.

## 빌드, 테스트, 설치

CMake 3.24 이상, C++20 컴파일러, Qt **6.8.3** Core 개발 패키지가 필요하다. Qt 버전은 CMake에서 정확히 일치해야 한다. macOS에 `/Volumes/Storage/Qt/6.8.3/macos`가 있으면 설치 스크립트가 자동으로 사용한다.

```sh
./install.sh
```

스크립트는 고정된 `build/`에서 Release 구성과 빌드, CTest를 수행한 다음 `$HOME/.local/SDK/iiSocietySync`에 설치한다. 이어서 `tests/consumer`를 `build/consumer/build/`에 독립 구성하고, 설치된 CMake 패키지와 헤더 및 동적 라이브러리만 사용하여 빌드하고 실행한다. 테스트는 반환 문자열, C++20 사용 여부, Qt 컴파일 버전 및 런타임 버전 6.8.3을 검증한다.

경로는 환경변수로 지정하며, 스크립트는 별도의 명령행 인자를 받지 않는다. `CMAKE_PREFIX_PATH`에 여러 경로를 전달할 때에는 세미콜론으로 구분한다.

```sh
QT_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
INSTALL_PREFIX="$HOME/.local/SDK/iiSocietySync" \
CMAKE_PREFIX_PATH="/additional/cmake/prefix" ./install.sh
```

수동 빌드와 검증도 동일한 `build/`를 사용한다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

## 설치 결과와 소비자 연결

기본 설치 경로 아래에 `include/iiSocietySync.h`, `lib/`의 공유 라이브러리, `lib/cmake/iiSocietySync/`의 Config·ConfigVersion·Targets, `share/iiSocietySync/README.md`가 생성된다. Windows 런타임 DLL은 `bin/`에 설치된다. 기본 설치 경로는 이 SDK가 최상위 CMake 프로젝트일 때만 적용한다. 직접 CMake로 구성할 때 HOME이 없으면 USERPROFILE을 기본 설치 경로 기준으로 사용한다.

```cmake
find_package(iiSocietySync 0.1.0 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE iiSocietySync::iiSocietySync)
```

소비자는 `CMAKE_PREFIX_PATH`에 SDK 설치 경로와 Qt 6.8.3 경로를 함께 지정한다. 내보낸 타깃이 C++20 요구사항과 `Qt6::Core` 연결을 전달한다. 설치 라이브러리에는 연결한 외부 라이브러리의 런타임 검색 경로를 보존하도록 `INSTALL_RPATH_USE_LINK_PATH`를 설정한다. Qt는 별도 설치된 런타임을 사용하며 복사하여 배포하지 않는다. Qt 사용과 재배포에는 해당 Qt 설치본의 라이선스 조건을 적용한다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiSocietySync의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용으로
배포한다. 전체 조건은 [LICENSE](LICENSE)를 따른다.

Qt를 포함한 외부 라이브러리와 별도 고지가 있는 서드파티 코드는 각자의 라이선스를
유지한다. 이 프로젝트의 라이선스 선언은 해당 서드파티 라이선스를 대체하지 않는다.
