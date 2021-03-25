# Starting the cluster

A lot of heartburn was generated getting here because minikube made it too
easy. Fine we'll setup a "production" cluster.

  vagrant up

That starts our "target" machine
Then find the IP address that vagrant instance is running on for the bridged network

  vagrant ssh
  ip addr

There was this path I took were I used kubespray locally rather than fighting
their vagrant file for a 3 node cluster. Then I ran across k0s. This makes it
almost as easy as minikube to get a kluster up with metallb (yet another dark
rabbit hole) setup that can respond to traffic. Behold the way my friends. You
don't want to fight all of Kubernetes? Behold the way.

* Modify k0sctl.yaml for the IP addresses of your vagrant instance
* k0sctl apply -c k0sctl.yaml
* k0sctl kubeconfig > admin.conf
* export KUBECONFIG=$(pwd)/admin.conf
* Victory
