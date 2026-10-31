package recording

import (
	"fmt"
	"os"
	"path/filepath"
)

// LockRoot prevents a live server and an importer from owning the same files.
// The OS releases the lock even after an unclean process exit.
func LockRoot(root string)(*os.File,error){if err:=os.MkdirAll(root,0755);err!=nil{return nil,err};f,err:=os.OpenFile(filepath.Join(root,"recordings.lock"),os.O_CREATE|os.O_RDWR,0600);if err!=nil{return nil,err};if err=lockFile(f);err!=nil{f.Close();return nil,fmt.Errorf("recording directory is already owned by another process: %w",err)};return f,nil}
