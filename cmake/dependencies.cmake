# Reference libraries for the course in docs/advanced-cpp-via-entt-and-taskflow.md.
#
# Pinned to the exact commits the course text was written and validated against,
# so every file/line reference in the document matches what you read here. Do not
# bump these casually: the quoted fragments in the course are only guaranteed
# correct at these SHAs.
#
# Sources land in third_party/ (gitignored) rather than the usual build/_deps so
# they survive `rm -rf build`. This course is mostly about *reading* them.

include(FetchContent)

# 2026-07-22 -- master in development toward v4 (version.h reads 4.0.0, untagged).
set(ACPP_ENTT_COMMIT 85c6bba014049b5de8fad49d25424df2f1f6a8c1)
# 2026-07-28 -- TF_VERSION 400100 => 4.1.0.
set(ACPP_TASKFLOW_COMMIT c4da2a49cd82f0c8ba5fad7e615a4017a38f88c5)

# Taskflow builds its unit tests and examples by default -- hundreds of TUs we do
# not want in this project's build. Opt out before MakeAvailable. To build them
# for study, configure the third_party/taskflow checkout separately.
set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG ${ACPP_ENTT_COMMIT}
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/entt
    GIT_PROGRESS ON
)

FetchContent_Declare(
    taskflow
    GIT_REPOSITORY https://github.com/taskflow/taskflow.git
    GIT_TAG ${ACPP_TASKFLOW_COMMIT}
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/taskflow
    GIT_PROGRESS ON
)

# Provides the interface targets EnTT::EnTT and Taskflow::Taskflow.
FetchContent_MakeAvailable(entt taskflow)
