#!/bin/sh

export KUBECONFIG=$(pwd)/admin.conf
k0sctl kubeconfig >| admin.conf
kubectl create secret generic rndc-key --from-file=rfc2136_tsig_secret=./rndc-key
kubectl apply -k ./kustomize/env/dev && \
  kubectl wait --for=condition=ready certificate kaffi-homelab-ca && \
  kubectl get secret kaffi-homelab-ca -o jsonpath="{.data.ca\.crt}" | base64 -d - > ca.crt
vagrant ssh -- "sudo /bin/sh -c \"cp /vagrant/ca.crt /usr/local/share/ca-certificates && update-ca-certificates && mkdir -p /etc/docker/certs.d/dockerhub.kaffi.home:5000/ && cp /vagrant/ca.crt /etc/docker/certs.d/dockerhub.kaffi.home:5000/\""
sudo cp ca.crt /usr/local/share/ca-certificates/kaffi.ca.crt
sudo cp ca.crt /etc/docker/certs.d/dockerhub.kaffi.home:5000/
sudo update-ca-certificates

helm repo add k8s-at-home https://k8s-at-home.com/charts/
helm repo add bitnami https://charts.bitnami.com/bitnami
helm repo add influxdata https://helm.influxdata.com/
helm repo add traefik https://helm.traefik.io/traefik
helm repo update

helm install kaffi-registry ./charts/kaffi-registry
helm install eclipse-mqtt ./charts/eclipse-mqtt -n iot --create-namespace
helm install iot ./charts/iot -n iot --create-namespace
