echo "Creating local Base RootFS..."
mkdir -p ./vm/base/debian-rootfs
wget -O ./vm/base/debian-rootfs.tar.gz https://cdimage.debian.org/cdimage/openstack/current/debian-12-genericcloud-amd64.tar.gz
tar -xzf ./vm/base/debian-rootfs.tar.gz -C ./vm/base/debian-rootfs
rm ./vm/base/debian-rootfs.tar.gz
echo "Base RootFS created successfully."
