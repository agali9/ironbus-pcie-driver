// Jenkinsfile — EDU PCI Driver CI Pipeline
//
// Stages:
//   1. Build kernel module
//   2. Build userspace library + tests
//   3. Boot QEMU VM with EDU device
//   4. Load kernel module inside VM
//   5. Run GoogleTest smoke tests
//   6. Run pytest smoke tests
//   7. Collect results + artifacts
//
// Requirements on the Jenkins agent:
//   - qemu-system-x86_64 installed
//   - kernel build tools (make, gcc, linux-headers)
//   - g++ with C++17 support
//   - libgtest-dev installed
//   - python3 + pytest installed
//   - SSH access to the QEMU VM on localhost:2222
//     (VM image at $VM_IMAGE_PATH, user: $VM_USER)
//
// Environment variables (set in Jenkins credentials / global config):
//   VM_IMAGE_PATH  — path to vm.qcow2
//   OVMF_CODE      — path to OVMF_CODE_4M.fd
//   OVMF_VARS      — path to OVMF_VARS_4M.fd
//   VM_USER        — SSH username inside VM (e.g. "ani")
//   VM_SSH_KEY     — Jenkins credential ID for the VM SSH private key

pipeline {

    agent any

    environment {
        REPO_ROOT    = "${WORKSPACE}"
        KERNEL_DIR   = "${WORKSPACE}/kernel"
        USER_DIR     = "${WORKSPACE}/userspace"
        RESULTS_DIR  = "${WORKSPACE}/results"
        VM_PORT      = "2222"
        // SSH shorthand used in all remote steps
        SSH_CMD      = "ssh -i \$VM_SSH_KEY_FILE -o StrictHostKeyChecking=no \
                            -o ConnectTimeout=30 \
                            ${VM_USER}@localhost -p ${VM_PORT}"
        SCP_CMD      = "scp -i \$VM_SSH_KEY_FILE -o StrictHostKeyChecking=no \
                            -P ${VM_PORT}"
    }

    options {
        timeout(time: 30, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '20'))
        ansiColor('xterm')
    }

    stages {

        // -------------------------------------------------------
        // Stage 1: Build kernel module on the agent
        // The agent must have matching kernel headers installed.
        // -------------------------------------------------------
        stage('Build: Kernel Module') {
            steps {
                echo '=== Building kernel/edu_pci.ko ==='
                sh '''
                    cd ${KERNEL_DIR}
                    make clean
                    make -j$(nproc)
                    ls -lh edu_pci.ko
                '''
            }
            post {
                success {
                    archiveArtifacts artifacts: 'kernel/edu_pci.ko',
                                     fingerprint: true
                }
            }
        }

        // -------------------------------------------------------
        // Stage 2: Build userspace library + GoogleTest binary
        // -------------------------------------------------------
        stage('Build: Userspace') {
            steps {
                echo '=== Building libedu_device.so + GoogleTest binary ==='
                sh '''
                    cd ${USER_DIR}
                    make clean
                    make -j$(nproc)
                    ls -lh libedu_device.so tests/test_edu_device demo
                '''
            }
            post {
                success {
                    archiveArtifacts artifacts: 'userspace/libedu_device.so',
                                     fingerprint: true
                }
            }
        }

        // -------------------------------------------------------
        // Stage 3: Start QEMU VM with EDU device
        // Boots in background; subsequent stages SSH into it.
        // -------------------------------------------------------
        stage('VM: Start QEMU') {
            steps {
                echo '=== Launching QEMU with -device edu ==='
                withCredentials([sshUserPrivateKey(
                        credentialsId: "${VM_SSH_KEY}",
                        keyFileVariable: 'VM_SSH_KEY_FILE')]) {
                    sh '''
                        mkdir -p ${RESULTS_DIR}

                        # Launch QEMU in the background
                        /usr/bin/qemu-system-x86_64 \
                            -m 2G \
                            -machine q35 \
                            -nographic \
                            -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE} \
                            -drive if=pflash,format=raw,file=${OVMF_VARS} \
                            -drive file=${VM_IMAGE_PATH},format=qcow2 \
                            -device edu \
                            -netdev user,id=net0,hostfwd=tcp::${VM_PORT}-:22 \
                            -device virtio-net-pci,netdev=net0 \
                            -pidfile ${RESULTS_DIR}/qemu.pid \
                            -daemonize

                        echo "QEMU PID: $(cat ${RESULTS_DIR}/qemu.pid)"

                        # Wait for SSH to become available (up to 120s)
                        for i in $(seq 1 24); do
                            if ${SSH_CMD} "echo VM_READY" 2>/dev/null; then
                                echo "VM is up after $((i*5))s"
                                break
                            fi
                            echo "Waiting for VM SSH... attempt $i/24"
                            sleep 5
                        done

                        # Final check — fail stage if VM never came up
                        ${SSH_CMD} "echo VM_CONFIRMED_UP"
                    '''
                }
            }
        }

        // -------------------------------------------------------
        // Stage 4: Copy artifacts into VM + load kernel module
        // -------------------------------------------------------
        stage('VM: Load Driver') {
            steps {
                withCredentials([sshUserPrivateKey(
                        credentialsId: "${VM_SSH_KEY}",
                        keyFileVariable: 'VM_SSH_KEY_FILE')]) {
                    sh '''
                        echo "=== Copying artifacts to VM ==="
                        ${SCP_CMD} ${KERNEL_DIR}/edu_pci.ko \
                            ${VM_USER}@localhost:/tmp/edu_pci.ko

                        ${SCP_CMD} -r ${USER_DIR}/ \
                            ${VM_USER}@localhost:/tmp/userspace/

                        echo "=== Verifying EDU PCI device visible in VM ==="
                        ${SSH_CMD} "lspci -nn | grep 1234:11e8"

                        echo "=== Loading kernel module ==="
                        ${SSH_CMD} "sudo rmmod edu_pci 2>/dev/null || true"
                        ${SSH_CMD} "sudo insmod /tmp/edu_pci.ko"
                        ${SSH_CMD} "ls -l /dev/edu_pci"
                        ${SSH_CMD} "dmesg | tail -5"
                    '''
                }
            }
        }

        // -------------------------------------------------------
        // Stage 5: Run GoogleTest suite inside VM
        // -------------------------------------------------------
        stage('Test: GoogleTest') {
            steps {
                withCredentials([sshUserPrivateKey(
                        credentialsId: "${VM_SSH_KEY}",
                        keyFileVariable: 'VM_SSH_KEY_FILE')]) {
                    sh '''
                        echo "=== Running GoogleTest ==="
                        ${SSH_CMD} "
                            cd /tmp/userspace &&
                            sudo LD_LIBRARY_PATH=. \
                                ./tests/test_edu_device \
                                --gtest_output=xml:/tmp/gtest_results.xml \
                                --gtest_color=yes
                        "

                        # Pull results back to agent for JUnit reporting
                        ${SCP_CMD} \
                            ${VM_USER}@localhost:/tmp/gtest_results.xml \
                            ${RESULTS_DIR}/gtest_results.xml
                    '''
                }
            }
            post {
                always {
                    junit testResults: 'results/gtest_results.xml',
                          allowEmptyResults: false
                }
            }
        }

        // -------------------------------------------------------
        // Stage 6: Run pytest suite inside VM
        // -------------------------------------------------------
        stage('Test: pytest') {
            steps {
                withCredentials([sshUserPrivateKey(
                        credentialsId: "${VM_SSH_KEY}",
                        keyFileVariable: 'VM_SSH_KEY_FILE')]) {
                    sh '''
                        echo "=== Running pytest ==="
                        ${SSH_CMD} "
                            cd /tmp/userspace &&
                            sudo PYTHONPATH=. python3 -m pytest \
                                python/test_edu_device.py \
                                -v \
                                --tb=short \
                                --junitxml=/tmp/pytest_results.xml
                        "

                        ${SCP_CMD} \
                            ${VM_USER}@localhost:/tmp/pytest_results.xml \
                            ${RESULTS_DIR}/pytest_results.xml
                    '''
                }
            }
            post {
                always {
                    junit testResults: 'results/pytest_results.xml',
                          allowEmptyResults: false
                }
            }
        }

        // -------------------------------------------------------
        // Stage 7: Collect dmesg + debugfs stats
        // -------------------------------------------------------
        stage('Collect: Logs + Stats') {
            steps {
                withCredentials([sshUserPrivateKey(
                        credentialsId: "${VM_SSH_KEY}",
                        keyFileVariable: 'VM_SSH_KEY_FILE')]) {
                    sh '''
                        echo "=== Collecting dmesg ==="
                        ${SSH_CMD} "dmesg | grep -i edu" \
                            > ${RESULTS_DIR}/dmesg_edu.txt || true

                        echo "=== Collecting debugfs stats (Phase 6) ==="
                        ${SSH_CMD} \
                            "cat /sys/kernel/debug/edu_pci/stats 2>/dev/null || echo 'debugfs not mounted'" \
                            > ${RESULTS_DIR}/debugfs_stats.txt || true
                    '''
                }
            }
            post {
                always {
                    archiveArtifacts artifacts: 'results/**',
                                     allowEmptyArchive: true
                }
            }
        }
    }

    // -------------------------------------------------------
    // Post: always shut down QEMU cleanly
    // -------------------------------------------------------
    post {
        always {
            sh '''
                if [ -f ${RESULTS_DIR}/qemu.pid ]; then
                    QPID=$(cat ${RESULTS_DIR}/qemu.pid)
                    echo "Stopping QEMU PID ${QPID}"
                    kill ${QPID} 2>/dev/null || true
                fi
            '''
        }
        success {
            echo 'All stages passed.'
        }
        failure {
            echo 'Pipeline failed — check stage logs above.'
        }
    }
}
