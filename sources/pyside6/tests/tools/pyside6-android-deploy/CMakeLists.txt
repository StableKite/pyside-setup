if(CMAKE_HOST_UNIX)
    PYSIDE_TEST(test_pyside6_android_deploy.py)
    PYSIDE_TEST(test_android_free_threading.py)
endif()
