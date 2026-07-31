package internal

import (
	"fmt"
	"os/exec"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc/mgr"
)

const SERVICE_NAME = "FaceLoginService"

// ServiceExists checks if the named service is installed.
func ServiceExists(name string) (bool, error) {
	m, err := mgr.Connect()
	if err != nil {
		return false, fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(name)
	if err != nil {
		return false, nil
	}
	s.Close()
	return true, nil
}

// stopService kills the service process and waits for it to stop.
func stopService(name string) {
	// Direct kill — no Control(svc.Stop) needed
	exec.Command("taskkill", "/f", "/im", "FaceLoginService.exe").Run()
	time.Sleep(2 * time.Second)
}

// StopAndDeleteService stops (if running) and deletes the service.
func StopAndDeleteService() error {
	exists, err := ServiceExists(SERVICE_NAME)
	if err != nil {
		return err
	}
	if !exists {
		return nil
	}

	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Stop and wait
	stopService(SERVICE_NAME)

	// Open fresh handle for deletion
	s, err := m.OpenService(SERVICE_NAME)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return nil
		}
		return fmt.Errorf("open for delete: %w", err)
	}

	err = s.Delete()
	s.Close()
	if err != nil {
		return fmt.Errorf("delete service: %w", err)
	}

	// Wait for deletion to take effect (SCM is async)
	for i := 0; i < 10; i++ {
		time.Sleep(500 * time.Millisecond)
		exists2, _ := ServiceExists(SERVICE_NAME)
		if !exists2 {
			return nil
		}
	}

	return nil
}

// InstallService creates (or updates) and starts the service.
func InstallService(exePath string) error {
	// If the service is already running, stop it so the new binary can start
	stopService(SERVICE_NAME)

	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Try to create; if exists, open for update
	s, err := m.CreateService(
		SERVICE_NAME,
		exePath,
		mgr.Config{
			StartType:    mgr.StartAutomatic,
			ErrorControl: mgr.ErrorNormal,
			DisplayName:  "FaceLogin 人脸认证服务",
			Description:  "FaceLogin — 基于人脸识别的 Windows 登录认证服务",
			ServiceType:  windows.SERVICE_WIN32_OWN_PROCESS,
		},
	)
	if err != nil {
		// Service already exists — open and update config
		s, err = m.OpenService(SERVICE_NAME)
		if err != nil {
			return fmt.Errorf("open existing service: %w", err)
		}
		defer s.Close()

		cfg, err := s.Config()
		if err != nil {
			return fmt.Errorf("get service config: %w", err)
		}
		cfg.BinaryPathName = exePath
		cfg.StartType = mgr.StartAutomatic
		if err := s.UpdateConfig(cfg); err != nil {
			return fmt.Errorf("update service config: %w", err)
		}
	} else {
		defer s.Close()
	}

	// Start the service
	err = s.Start()
	if err != nil && err != windows.ERROR_SERVICE_ALREADY_RUNNING {
		return fmt.Errorf("start service: %w", err)
	}
	return nil
}
