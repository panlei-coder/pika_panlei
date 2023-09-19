FETCHCONTENT_DECLARE(
        leveldb
        GIT_REPOSITORY https://github.com/google/leveldb.git
        GIT_TAG main
)
SET(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
SET(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
SET(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
FETCHCONTENT_MAKEAVAILABLE(leveldb)
