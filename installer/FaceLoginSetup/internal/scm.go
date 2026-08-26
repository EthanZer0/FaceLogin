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

// stopService stops the service process and waits for the SCM state to settle
// to STOPPED.
//
// FaceLoginService does NOT respond to SCM graceful-stop controls (sc stop /
// ControlService(SERVICE_CONTROL_STOP) hang until timeout) — the process runs
// its own main loop and doesn't handle SERVICE_ACCEPT_STOP. So we kill the
// process directly (fast, reliable), then WAIT for the SCM to observe the exit
// and mark the service STOPPED.
//
// Waiting matters: DeleteService requires the service to be stopped. Killing
// the process and deleting immediately races the SCM state update and returns
// ERROR_SERVICE_MARKED_FOR_DELETE (1072), which wedges the service until
// reboot — the bug this code fixes.
func stopService(s *mgr.Service) error {
	exec.Command("taskkill", "/f", "/im", "FaceLoginService.exe").Run()

	// Poll until the SCM reports STOPPED (the killed process's exit propagates
	// to SCM within a moment). Give it up to ~5s.
	for i := 0; i < 10; i++ {
		time.Sleep(500 * time.Millisecond)
		st, err := s.Query()
		if err != nil {
			// Service gone entirely — even better than stopped.
			if err == windows.ERROR_SERVICE_DOES_NOT_EXIST ||
				err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
				return nil
			}
			return err
		}
		if st.State == windows.SERVICE_STOPPED {
			return nil
		}
	}

	// Process may have been killed but SCM is slow; proceed anyway — Delete
	// after a killed process usually succeeds once SCM catches up.
	return nil
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

	// Open the service and stop it gracefully via SCM.
	s, err := m.OpenService(SERVICE_NAME)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return nil
		}
		return fmt.Errorf("open service: %w", err)
	}

	stopErr := stopService(s)

	// Delete the service. If a previous delete attempt left the service marked
	// for deletion (ERROR_SERVICE_MARKED_FOR_DELETE 1072), DeleteService returns
	// this error — but the service IS being removed: once every handle is
	// closed and the process is gone, SCM completes the deletion. The old code
	// surfaced 1072 as a hard failure ("uninstall keeps erroring"), because it
	// never closed the handle and re-checked. So on 1072 we close, wait, and
	// confirm the service actually disappeared before reporting an error.
	err = s.Delete()
	s.Close()
	if err != nil {
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			// Close released the handle above. Give SCM a few seconds to finish
			// removing a marked-for-delete service, then re-check existence.
			for i := 0; i < 20; i++ {
				time.Sleep(500 * time.Millisecond)
				exists2, _ := ServiceExists(SERVICE_NAME)
				if !exists2 {
					return nil // service removed — the marked-for-delete was transient
				}
			}
			return fmt.Errorf("service is marked for deletion but removal did not finish; restart Windows and retry")
		}
		if stopErr != nil {
			return fmt.Errorf("delete service (after stop failed: %v): %w", stopErr, err)
		}
		return fmt.Errorf("delete service: %w", err)
	}

	// Wait for deletion to take effect (SCM is async).
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
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// If the service already exists, stop it gracefully so the new binary can
	// be updated/started (a running service holds the EXE file open).
	if exists, _ := ServiceExists(SERVICE_NAME); exists {
		if s, err := m.OpenService(SERVICE_NAME); err == nil {
			_ = stopService(s)
			s.Close()
		}
	}

	// Try to create; if exists, open for update
	s, err := m.CreateService(
		SERVICE_NAME,
		exePath,
		mgr.Config{
			StartType:    mgr.StartAutomatic,
			ErrorControl: mgr.ErrorNormal,
			DisplayName:  "FaceLogin Face Authentication Service",
			Description:  "FaceLogin service for local face-authenticated Windows sign-in",
			ServiceType:  windows.SERVICE_WIN32_OWN_PROCESS,
		},
	)
	if err != nil {
		// A service left "marked for deletion" (from an interrupted uninstall)
		// fails both CreateService and OpenService with 1072. It disappears on
		// its own once every handle is closed and the process is gone — wait
		// for that, then retry the create once.
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			for i := 0; i < 20; i++ {
				time.Sleep(500 * time.Millisecond)
				exists2, _ := ServiceExists(SERVICE_NAME)
				if !exists2 {
					break
				}
			}
			s, err = m.CreateService(
				SERVICE_NAME,
				exePath,
				mgr.Config{
					StartType:    mgr.StartAutomatic,
					ErrorControl: mgr.ErrorNormal,
					DisplayName:  "FaceLogin Face Authentication Service",
					Description:  "FaceLogin service for local face-authenticated Windows sign-in",
					ServiceType:  windows.SERVICE_WIN32_OWN_PROCESS,
				},
			)
			if err != nil {
				return fmt.Errorf("create service (retry after marked-for-delete): %w", err)
			}
		} else {
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
		}
	} else {
		defer s.Close()
	}

	// Configure crash-recovery: if the service process dies without reporting
	// SERVICE_STOPPED (a crash), SCM restarts it automatically — up to 3 times,
	// 5s apart. Without this, a one-time service crash leaves face login dead
	// until the next reboot or manual restart (issue #10). The failure counter
	// resets after 1 day without a failure, so a permanent fault still stops
	// looping. We set this AFTER create/update so an upgrade overwrites the
	// prior recovery config with the current one.
	if err := configureServiceRecovery(s); err != nil {
		// Non-fatal: the service still starts without recovery configured.
		// Log and continue rather than failing the whole install.
		fmt.Printf("warning: failed to set service recovery actions: %v\n", err)
	}

	// Start the service
	err = s.Start()
	if err != nil && err != windows.ERROR_SERVICE_ALREADY_RUNNING {
		return fmt.Errorf("start service: %w", err)
	}
	return nil
}

// configureServiceRecovery sets the SCM failure actions for the FaceLogin
// service: on a crash (process exit without SERVICE_STOPPED), restart the
// service up to 3 times with a 5s delay each; reset the failure counter after
// 1 day (86400s) of no failures.
//
// FailureActionsOnNonCrashFailures must be TRUE too: by default SCM only runs
// the recovery actions when the process dies WITHOUT reporting SERVICE_STOPPED.
// Terminating the process via Task Manager is treated by SCM as a "non-crash"
// failure (exit code 0), so without this flag the restart never fires.
// NOTE: this flag (and the actions) only take effect after the next system
// restart — documented ChangeServiceConfig2 behavior.
func configureServiceRecovery(s *mgr.Service) error {
	actions := []mgr.RecoveryAction{
		{Type: mgr.ServiceRestart, Delay: 5 * time.Second},
		{Type: mgr.ServiceRestart, Delay: 5 * time.Second},
		{Type: mgr.ServiceRestart, Delay: 5 * time.Second},
	}
	if err := s.SetRecoveryActions(actions /*resetPeriod=*/, 86400); err != nil {
		return err
	}
	return s.SetRecoveryActionsOnNonCrashFailures(true)
}
