//   Copyright 2012 - Global Vision Systems
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

/*
 * ComponentManager.cpp
 *
 *  Created on: 4 mai 2012
 *      Author: simon
 */
#include <iostream>
#include "ComponentManagerImpl.h"
#include <usLDAPFilter.h>
US_USE_NAMESPACE
namespace ds4cpp
{
ComponentManagerImpl::ComponentManagerImpl(ModuleContext* context) : context(context)
{
    context->AddServiceListener(this, &ComponentManagerImpl::handleServiceEvent) ;
    context->AddModuleListener(this, &ComponentManagerImpl::handleModuleEvent) ;
}

ComponentManagerImpl::~ComponentManagerImpl()
{
    context->RemoveServiceListener(this, &ComponentManagerImpl::handleServiceEvent) ;
    context->RemoveModuleListener(this, &ComponentManagerImpl::handleModuleEvent) ;
}

void ComponentManagerImpl::handleModuleEvent(ModuleEvent event)
{
	ModuleEvent::Type mt = event.GetType() ;
	if (event.GetType() == ModuleEvent::UNLOADING)
	{
		this->removeModuleComponents(event.GetModule()) ;
	}
}

std::list<ComponentInstance*> ComponentManagerImpl::getInstanceListeningService(const std::string& service, const us::ServiceReference& ref)
{
	std::list<ComponentInstance*> listeningInstancies ;

	// Retrieve components that has reference on the incoming service
    std::list<ComponentInstance*>* ComponentInstances = componentReferences[service] ;

	// Valid ?
    if (ComponentInstances)
    {
        for (auto compoInstIt = ComponentInstances->begin() ;
                compoInstIt != ComponentInstances->end() ; ++compoInstIt)
        {
			// Find the reference to know if there is a target
			for (auto referenceIt = (*compoInstIt)->getResolvedReferences().begin();
				referenceIt != (*compoInstIt)->getResolvedReferences().end(); ++referenceIt)
			{
				// Is it the good reference ?
				if (referenceIt->interface == service)
				{
					bool bind = true ;

					// Is there a target ?
					if (!referenceIt->target.empty())
					{
						try
						{
							LDAPFilter filter(referenceIt->target) ;
							bind = filter.Match(ref) ;
						}
						catch (std::exception e)
						{
							US_ERROR << "Error on LDAP filter : " << e.what() ;
							bind = false ;
						}
					}

					// Filter match ? (or is empty)
					if (bind)
					{
						listeningInstancies.push_back(*compoInstIt) ;
					}
				}
			}
        }
    }
	return listeningInstancies ;
}

std::list<ComponentInstance*> ComponentManagerImpl::getInstanceProvidingAService(const ComponentReference& reference, const std::string& requireService)
{
	std::list<ComponentInstance *> matchList ;
	for (auto componentIt = components.begin(); componentIt != components.end(); ++componentIt)
	{
		// Do a preliminary filter on provided interface and after check about reference filter
		for (auto componentProvidedServiceIt = (*componentIt)->descriptor.providedServices->begin(); componentProvidedServiceIt != (*componentIt)->descriptor.providedServices->end(); ++componentProvidedServiceIt)
		{
			if (requireService == (*componentProvidedServiceIt))
			{
				// Loop on each component instance to get matching instance
				for (auto compInstIt = (*componentIt)->instances.begin(); compInstIt != (*componentIt)->instances.end(); ++compInstIt)
				{
					bool match = true ;

					// Is there a target ?
					if (!reference.target.empty())
					{
						// Apply ldap filter
						try
						{
							LDAPFilter filter(reference.target) ;
							match = filter.Match((*compInstIt)->getProperties()) ;
						}
						catch (std::exception e)
						{
							US_ERROR << "Error on LDAP filter : " << e.what() ;
							match = false ;
						}	
					}
					if (match)
					{
						matchList.push_back(*compInstIt) ;
					}
				}
			}
		}
	}
	return matchList ;
}

bool ComponentManagerImpl::getValidServiceRef(const std::list<ComponentInstance *>& providingInstance, const std::string& interface, us::ServiceReference currentServiceRef, us::ServiceReference& replacingServiceReference)
{
	for (std::list<ComponentInstance *>::const_iterator instIt = providingInstance.begin(); instIt != providingInstance.end(); ++instIt)
	{
		std::map<std::string, us::ServiceRegistration>::iterator it = (*instIt)->providedServiceRegistration.find(interface) ;
		if (it != (*instIt)->providedServiceRegistration.end() && it->second.GetReference() != currentServiceRef)
		{
			replacingServiceReference = it->second.GetReference() ;
			return true ;
		}
	}
	return false ;
}

void ComponentManagerImpl::referenceHasLeft(ComponentInstance *instance, const std::string& interface, const us::ServiceReference& ref)
{
	for (auto refsIt = instance->getResolvedReferences().begin(); refsIt != instance->getResolvedReferences().end(); ++refsIt)
	{
		// If there is several reference on this interface it may have a bug here
		if (refsIt->interface == interface)
		{
			if (refsIt->policy == ComponentReference::DYNAMIC && refsIt->cardinality == ComponentReference::SINGLE)
			{
				// Policy is dynamic we are looking for a new reference
				std::list<ComponentInstance *> providingInstance = getInstanceProvidingAService(*refsIt, interface) ;
				us::ServiceReference replacingServiceRef ;
				if (getValidServiceRef(providingInstance, interface, ref, replacingServiceRef))
				{
					// We want the component be able to live during the replacing of the interface, so we call bind before unbind
					instance->bindService(interface, refsIt->name, replacingServiceRef) ;
					instance->unbindService(interface, refsIt->name, ref) ;
				}
				else 
                { // No replacing service

					if (refsIt->type == ComponentReference::MANDATORY_REF && instance->serviceReferences.count(interface) == 1)
					{
						// Dead...
						outcomingComponentInstance(instance) ;
						return ;
					}
					else
					{
						// It remains several interface of the service or it is not a mandatory service we can let the component living
						instance->unbindService(interface, refsIt->name, ref) ;
					}
				}
			}
			else
			{
				if (refsIt->type == ComponentReference::MANDATORY_REF && refsIt->cardinality == ComponentReference::SINGLE)
				{
					// As it is not a dynamic reference we don't try to replace it
					outcomingComponentInstance(instance) ;
					return ;
				}
				else
				{
					// Multiple cardinality, if the instance is unsatisfied it will be managed by the ComponentInstance himself
					instance->unbindService(interface, refsIt->name, ref) ;
				}
			}

		}
	}
}

void ComponentManagerImpl::handleServiceEvent(ServiceEvent event)
{
    if (event.GetType() == ServiceEvent::REGISTERED)
    {
		// New incoming service 
        std::list<string> interfaces = any_cast<std::list<string> >(event.GetServiceReference().GetProperty(ServiceConstants::OBJECTCLASS())) ;
        US_INFO << "New Service : " << interfaces ;

        // For each objectClass provided by the service object
        // find the appropriate components who want it and give it
        for (auto objClassIt = interfaces.begin() ; objClassIt != interfaces.end() ; ++objClassIt)
        {
			// Retrieve the list of service that are listening to this leaving service
			auto listeningComponents = getInstanceListeningService(*objClassIt, event.GetServiceReference()) ;
			for (auto lsIt = listeningComponents.begin(); lsIt != listeningComponents.end(); ++lsIt)
			{
				for (auto refsIt = (*lsIt)->getResolvedReferences().begin(); refsIt != (*lsIt)->getResolvedReferences().end(); ++refsIt)
				{
					if (refsIt->interface == *objClassIt)
					{
						// Check if the cardinality is multiple
						// Or if it is single if there isn't already a reference on a service
						if (refsIt->cardinality == ComponentReference::MULTIPLE || 
							(refsIt->cardinality == ComponentReference::SINGLE && (*lsIt)->serviceReferences.count(refsIt->interface) == 0))
						{
							// Register the incoming service
							(*lsIt)->bindService(*objClassIt, refsIt->name, event.GetServiceReference()) ;
							// Enable the instance ?
							enableIfSatisfied(*lsIt) ;
						}
					}
				}
			}
        }
    }
    else if (event.GetType() == ServiceEvent::UNREGISTERING)
    {
		// Outcoming services 
        std::list<string> interfaces = any_cast<std::list<string> >(event.GetServiceReference().GetProperty(ServiceConstants::OBJECTCLASS())) ;
        US_INFO << "Outcoming Service : " << interfaces ;

        // For each objectClass provided by the service object
        // find the appropriate components who want it and withdraw it
        for (auto objClassIt = interfaces.begin(); objClassIt != interfaces.end() ; ++objClassIt)
        {
			// At the destruction of a module all registered services get unregistered, so if we found the component providing this service we had to delete it 
			// (otherwise it has already been removed in Component::unpublishServices or on a previous way here)
			//for (auto compIt = this->components.begin(); compIt != this->components.end(); ++compIt)
			//{
			//	bool find = false ;
			//	for (auto compInstIt = (*compIt)->instances.begin(); compInstIt != (*compIt)->instances.end(); ++compInstIt)
			//	{
			//		auto compInstProvidesServiceReferenceIt = (*compInstIt)->providedServiceRegistration.find(*objClassIt) ;
			//		if (compInstProvidesServiceReferenceIt != (*compInstIt)->providedServiceRegistration.end())
			//		{
			//			// The component is still alive, we have to destroy it
			//			(*compInstIt)->providedServiceRegistration.erase(compInstProvidesServiceReferenceIt) ; // We don't want to unregister this service twice
			//			this->outcomingComponentInstance((*compInstIt)) ;
			//			find = true ;
			//			break ;
			//		}
			//	}
			//	if (find)
			//		break ;
			//}

			// Retrieve the list of service that are listening to this leaving service
			auto listeningServices = getInstanceListeningService(*objClassIt, event.GetServiceReference()) ;
			for (auto lsIt = listeningServices.begin(); lsIt != listeningServices.end(); ++lsIt)
			{
				referenceHasLeft(*lsIt, *objClassIt, event.GetServiceReference()) ;
			}
		}
    }
}

void ComponentManagerImpl::outcomingComponentInstance(ComponentInstance *instance) 
{
	for (auto refIt = instance->getResolvedReferences().begin(); refIt != instance->getResolvedReferences().end(); ++refIt)
	{
		// Remove instance from component reference
		std::list<ComponentInstance*>* componentInstances = componentReferences[refIt->interface] ;
		for (auto refInstIt = componentInstances->begin(); refInstIt != componentInstances->end(); ++refInstIt)
		{
			if (*refInstIt == instance)
			{
				componentInstances->erase(refInstIt) ;
				break ;
			}
		}
		if (componentInstances->size() == 0)
		{
			componentReferences[refIt->interface] = NULL ;
			delete componentInstances ;
		}
	}
	// Unregister...
	instance->unregister() ;
}

void ComponentManagerImpl::removeModuleComponents(Module* module)
{
	for (std::vector<Component *>::iterator it = this->components.begin(); it != this->components.end(); )
	{
		if ((*it)->module == module)
		{
			Component *outcomingComponent = *it ;
			// Erase component and retrieve the next iterator
			std::vector<Component *>::iterator tmp = it ;
			it = this->components.erase(tmp) ;

			// Unregister instancies
			while (!outcomingComponent->instances.empty())
			{
				outcomingComponentInstance(outcomingComponent->instances.front()) ;		
			}
			
			// Free memory
			delete outcomingComponent ;
		}
		else
		{
			++it;
		}
	}
}

void ComponentManagerImpl::removeComponent(Module* module, const ComponentDescriptor& descriptor)
{
	US_DEBUG << "Outcoming component descriptor: " << descriptor.componentId << " provided by " << module->GetName() ;

	// Find the corresponding component
	Component* outcomingComponent = NULL ;
	for (std::vector<Component *>::iterator it = this->components.begin(); it != this->components.end(); ++it)
	{
		if ((*it)->descriptor.componentId == descriptor.componentId)
		{
			outcomingComponent = *it ;

			// Unregister component
			this->components.erase(it) ;
			break ;
		}
	}
	if (outcomingComponent != NULL)
	{
		// Unregister instancies
		while (!outcomingComponent->instances.empty())
		{
			outcomingComponentInstance(outcomingComponent->instances.front()) ;		
		}
		return ;
	}
	US_DEBUG << "Unknow outcoming component \"" << descriptor.componentId ;
}

void ComponentManagerImpl::newComponent(Module* module, const ComponentDescriptor& descriptor)
{
    US_DEBUG << "New component descriptor: " << descriptor.componentId << " provided by " << module->GetName() ;

	// Build the component
	Component *component = new Component(module, descriptor) ;
	this->components.push_back(component) ;     // TODO vector's thread-safety?

	// Need a factory
	if (!component->descriptor.singleton)
	{
		// Build a factory which will build component when user require it
		ComponentFactoryImplWrapper *factory = new ComponentFactoryImplWrapper(this, component) ;

		// Build the factory component and register it
		component = new Component(module, *factory->getFactoryDescriptor()) ;
		this->components.push_back(component) ;

		// Create factory instance
		ComponentInstance *factoryInstance = new ComponentInstance(component, *factory->getFactoryDescriptor()->properties) ;
		factoryInstance->set(reinterpret_cast<us::Base*>(factory)) ;
		component->factory = true ;
		component->instances.push_back(factoryInstance) ;
	}
	newComponentInstance(component) ;
}

ComponentInstance* ComponentManagerImpl::newComponentInstance(Component *component, const us::ServiceProperties& overrideProperties, const us::ServiceProperties& componentProperties)
{
	ComponentInstance *instance ;
	if (!component->factory)
	{
		// Create an empty instance
		instance = component->newEmptyComponentInstance(overrideProperties, componentProperties) ;
	}
	else
	{
		instance = component->instances.at(0) ; // Factory are singleton
	}

	// Update component references map
	auto        refs = instance->getResolvedReferences() ;
	for (auto it = refs.begin() ; it != refs.end() ; ++it)
	{
		if (it->interface.size() == 0)
		{
			return 0 ;
		}
		std::list<ComponentInstance*>* instances = componentReferences[it->interface] ;
		if (!instances)
		{
			instances                          = new std::list<ComponentInstance*> ;
			componentReferences[it->interface] = instances ;
		}
		instances->push_back(instance) ;
		US_DEBUG << "Adding " << component->descriptor.componentId << " to references map for service " << it->interface ;
	}

	injectAvailableReferencies(instance) ;
	return instance;
}

void ComponentManagerImpl::injectAvailableReferencies(ComponentInstance* instance)
{
    // try to find services already present
	for (auto it = instance->getResolvedReferences().begin() ;
         it != instance->getResolvedReferences().end() ; ++it)
    {
		// Target filter
		std::list<ServiceReference> refs = context->GetServiceReferences(it->interface, it->target) ;

        // take first available
        for (auto it2 = refs.begin(); it2 != refs.end(); ++it2)
        {
			instance->bindService(it->interface, it->name, *it2) ;
        }
    }

    // test if component is satisfied
    enableIfSatisfied(instance) ;
}

bool ComponentManagerImpl::enableIfSatisfied(ComponentInstance* instance)
{
	Component *component = instance->getComponent() ;
	if (instance->isSatisfied())
    {
		// If it is a singleton we can only have one instance
		if ((component->descriptor.singleton && component->getNumInstance() == 1) || // Because when we create the instance it has been registered
			 component->factory || !component->descriptor.singleton)
        {
			return instance->enableInstance() ;
        }
		else
		{
			// TODO: unregister instance
		}
    }
	return false ;
}
}

/* namespace ds4cpp */
