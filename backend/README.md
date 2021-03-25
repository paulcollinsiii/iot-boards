## k8s

Kustomize files intended to be used with `kubectl` to do first pass deployments
into `minikube`. Note that the mqtt folder should be build and published into
the minikube docker repo via `minikube docker-env`

## images/mqtt

Docker file for building based on the mosquitto docker image. This builds the
config file directly into the container, probably not the best initial setup,
but good enough for a first pass at throwing it into kubernetes

#kubespray

Vagrant and Ansible config to install a remotely accessible kubernetes
"cluster" Basically minikube, but such that my actual devices could potentially
hit the cluster.
