// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package cmd

import (
	"github.com/spf13/afero"
	"github.com/spf13/cobra"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/cli/cmd/acl"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/cli/cmd/common"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/config"
)

func NewACLCommand(fs afero.Fs, mgr config.Manager) *cobra.Command {
	var (
		brokers    []string
		configFile string
		user       string
		password   string
		mechanism  string
	)
	command := &cobra.Command{
		Use:          "acl",
		Short:        "Manage ACLs",
		SilenceUsage: true,
	}

	common.AddKafkaFlags(
		command,
		&configFile,
		&user,
		&password,
		&mechanism,
		&brokers,
	)

	configClosure := common.FindConfigFile(mgr, &configFile)
	brokersClosure := common.DeduceBrokers(
		common.CreateDockerClient,
		configClosure,
		&brokers,
	)
	kAuthClosure := common.KafkaAuthConfig(&user, &password, &mechanism)
	adminClosure := common.CreateAdmin(brokersClosure, configClosure, kAuthClosure)

	command.AddCommand(acl.NewCreateACLsCommand(adminClosure))
	return command
}
