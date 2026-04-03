#!/usr/bin/env bash
# =============================================================================
#  api_bundle_for_vm.sh
#  Run on WSL — packages binary + libs into a single tar.gz
#  Output: ~/FAISS-Actual-26June25/address-service-optimized/address-service-bundle.tar.gz
# =============================================================================
set -euo pipefail

SERVICE_DIR=~/FAISS-Actual-26June25/address-service-optimized
BUNDLE_DIR="${SERVICE_DIR}/address-service-bundle"
BUNDLE_TAR="${SERVICE_DIR}/address-service-bundle.tar.gz"

echo "==> Cleaning previous bundle..."
rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}/bin"
mkdir -p "${BUNDLE_DIR}/lib"
mkdir -p "${BUNDLE_DIR}/config"
mkdir -p "${BUNDLE_DIR}/logs"
mkdir -p "${BUNDLE_DIR}/uploads"

# =============================================================================
#  1. Binary
# =============================================================================
echo "==> Copying binary..."
cp "${SERVICE_DIR}/build/address-service" "${BUNDLE_DIR}/bin/"
chmod +x "${BUNDLE_DIR}/bin/address-service"
echo "   ✓ address-service binary"

# =============================================================================
#  2. Shared libraries
# =============================================================================
echo "==> Copying shared libraries..."

LIBS=(
    /usr/local/lib/libpostal.so.1.0.1
    /lib/x86_64-linux-gnu/libssl.so.3
    /lib/x86_64-linux-gnu/libcrypto.so.3
    /lib/x86_64-linux-gnu/libjsoncpp.so.25
    /lib/x86_64-linux-gnu/libuuid.so.1
    /lib/x86_64-linux-gnu/libpq.so.5
    /lib/x86_64-linux-gnu/libz.so.1
    /lib/x86_64-linux-gnu/libstdc++.so.6
    /lib/x86_64-linux-gnu/libgcc_s.so.1
    /lib/x86_64-linux-gnu/libgssapi_krb5.so.2
    /lib/x86_64-linux-gnu/libldap-2.5.so.0
    /lib/x86_64-linux-gnu/libkrb5.so.3
    /lib/x86_64-linux-gnu/libk5crypto.so.3
    /lib/x86_64-linux-gnu/libcom_err.so.2
    /lib/x86_64-linux-gnu/libkrb5support.so.0
    /lib/x86_64-linux-gnu/liblber-2.5.so.0
    /lib/x86_64-linux-gnu/libsasl2.so.2
    /lib/x86_64-linux-gnu/libgnutls.so.30
    /lib/x86_64-linux-gnu/libkeyutils.so.1
    /lib/x86_64-linux-gnu/libresolv.so.2
    /lib/x86_64-linux-gnu/libp11-kit.so.0
    /lib/x86_64-linux-gnu/libidn2.so.0
    /lib/x86_64-linux-gnu/libunistring.so.2
    /lib/x86_64-linux-gnu/libtasn1.so.6
    /lib/x86_64-linux-gnu/libnettle.so.8
    /lib/x86_64-linux-gnu/libhogweed.so.6
    /lib/x86_64-linux-gnu/libgmp.so.10
    /lib/x86_64-linux-gnu/libffi.so.8
    /lib/x86_64-linux-gnu/libm.so.6
)

for lib in "${LIBS[@]}"; do
    if [ -f "${lib}" ]; then
        cp "${lib}" "${BUNDLE_DIR}/lib/"
        echo "   ✓ $(basename ${lib})"
    else
        echo "   ✗ MISSING: ${lib}"
    fi
done

# Symlinks so binary resolves soname aliases
cd "${BUNDLE_DIR}/lib"
ln -sf libpostal.so.1.0.1  libpostal.so.1
ln -sf libpostal.so.1.0.1  libpostal.so
ln -sf libssl.so.3          libssl.so
ln -sf libcrypto.so.3       libcrypto.so
ln -sf libstdc++.so.6       libstdc++.so
ln -sf libgcc_s.so.1        libgcc_s.so
cd - > /dev/null
echo "   ✓ symlinks created"

# =============================================================================
#  3. Config — port 8090, standard libpostal data path on VM
# =============================================================================
echo "==> Copying config..."
cp "${SERVICE_DIR}/config/config.json" "${BUNDLE_DIR}/config/config.json"
sed -i 's/"port": 8080/"port": 8090/' "${BUNDLE_DIR}/config/config.json"
sed -i 's|"data_dir": ".*"|"data_dir": "/home/flureelabs/address-service/data/libpostal"|' \
    "${BUNDLE_DIR}/config/config.json"
echo "   ✓ config.json (port=8090, data_dir=standard libpostal)"

# =============================================================================
#  4. run.sh
# =============================================================================
echo "==> Creating run.sh..."
cat > "${BUNDLE_DIR}/run.sh" << 'EOF'
#!/usr/bin/env bash
# =============================================================================
#  run.sh — starts address-service with bundled libraries
#
#  To switch libpostal model — edit config/config.json, change data_dir:
#    Standard : /home/flureelabs/address-service/data/libpostal
#    Senzing  : /home/flureelabs/address-service/data/libpostal_senzing
#  Then: sudo systemctl restart address-service
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH:-}"
cd "${SCRIPT_DIR}"
exec "${SCRIPT_DIR}/bin/address-service" -c "${SCRIPT_DIR}/config/config.json"
EOF
chmod +x "${BUNDLE_DIR}/run.sh"
echo "   ✓ run.sh"

# =============================================================================
#  5. systemd service file
# =============================================================================
echo "==> Creating systemd service file..."
cat > "${BUNDLE_DIR}/address-service.service" << 'EOF'
[Unit]
Description=Address Normalization Service
After=network.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=flureelabs
WorkingDirectory=/home/flureelabs/address-service
ExecStart=/home/flureelabs/address-service/run.sh
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=address-service

[Install]
WantedBy=multi-user.target
EOF
echo "   ✓ address-service.service"

# =============================================================================
#  6. vm-setup.sh — drop this on VM and run it once
# =============================================================================
echo "==> Creating vm-setup.sh..."
cp "${SERVICE_DIR}/vm-setup.sh" "${BUNDLE_DIR}/vm-setup.sh" 2>/dev/null || true
chmod +x "${BUNDLE_DIR}/vm-setup.sh" 2>/dev/null || true

# =============================================================================
#  7. Create tar.gz inside address-service-optimized/
# =============================================================================
echo "==> Creating bundle tar.gz..."
cd "${SERVICE_DIR}"
tar -czf "${BUNDLE_TAR}" address-service-bundle/
rm -rf "${BUNDLE_DIR}"  # clean up folder, keep only tar.gz

echo ""
echo "============================================"
echo "  Bundle ready!"
echo "  Location : ${BUNDLE_TAR}"
echo "  Size     : $(du -sh ${BUNDLE_TAR} | cut -f1)"
echo "============================================"
echo ""
echo "WinSCP — drag and drop to VM /home/flureelabs/:"
echo "  1. address-service-bundle.tar.gz   (this file)"
echo "  2. ~/libpostal_data/libpostal/      (standard model data, ~1.5GB)"
echo "     → drop as: address-service-data"
echo ""
echo "Then on VM run:"
echo "  tar -xzf address-service-bundle.tar.gz"
echo "  bash address-service-bundle/vm-setup.sh"